#include "CanonicalTrace.hpp"
#include "EbHome.hpp"
#include "Requester.hpp"
#include <algorithm>
#include <filesystem>
#include <map>
#include <string>

using namespace compactchi;
class Context : public TLLocalContext {
public:
    TLLocalConfig cfg{};
    uint64_t cycles = 1;
    uint64_t cycle() const noexcept override { return cycles; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return cfg.seed; }
    const TLLocalConfig& config() const noexcept override { return cfg; }
    TLLocalConfig& config() noexcept override { return cfg; }
    Context() {
        cfg.seed = 42; cfg.memoryStart = 0x1000; cfg.memoryEnd = 0x2000;
        cfg.memoryReadDepth = 4; cfg.memoryWriteDepth = 2;
        cfg.memoryReadLatency = 3; cfg.memoryWriteLatency = 2;
        cfg.memoryCycleUnit = 1; cfg.memoryOOOR = 1;
    }
};
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static LineData initial(uint64_t address) {
    LineData data{};
    for (unsigned i = 0; i < 64; ++i) data[i] = uint8_t((address >> 6) ^ (i * 17) ^ 42);
    return data;
}
static CanonicalEvent event(Context& ctx, Channel channel, unsigned node, unsigned target,
                             unsigned txn, unsigned opcode, bool ready, bool tag) {
    return {ctx.cycles, ctx.cycles * 2, channel, node, target, txn, opcode, true, ready, tag};
}
static void request(CanonicalTrace& trace, Context& ctx, const EbRequest& r) {
    auto e = event(ctx, Channel::ChiTxReq, r.source, r.target, r.txn, r.opcode, true, r.traceTag);
    e.address = r.address; e.allowRetry = r.allowRetry; e.expCompAck = r.expCompAck;
    e.pCrdType = r.pCrdType; e.size = r.size; e.memAttr = r.memAttr; e.order = r.order;
    e.ns = r.ns; e.likelyShared = r.likelyShared; trace.record(e);
}
static void response(CanonicalTrace& trace, Context& ctx, Channel channel, const EbResponse& r, bool ready) {
    auto e = event(ctx, channel, r.source, r.target, r.txn, r.opcode, ready, r.traceTag);
    e.dbid = r.dbid; e.resp = r.resp; e.respErr = r.respErr; e.pCrdType = r.pCrdType;
    trace.record(e);
}
static void data(CanonicalTrace& trace, Context& ctx, Channel channel, const EbData& r, bool ready) {
    auto e = event(ctx, channel, r.source, r.target, r.txn, r.opcode, ready, r.traceTag);
    e.dbid = r.dbid; e.homeNode = r.home; e.dataId = r.dataId; e.resp = r.resp;
    e.respErr = r.respErr; e.byteEnable = r.byteEnable; e.data = r.data; trace.record(e);
}
static void initialize(Context& ctx, GlobalBoard<paddr_t>& board, chi_agent::HomeMemory& memory) {
    for (uint64_t address : {0x1000U, 0x1040U}) {
        const auto bytes = initial(address);
        auto entry = std::make_shared<Global_SBEntry>(); entry->status = Global_SBEntry::SB_VALID;
        entry->data = make_shared_tldata<64>();
        std::copy(bytes.begin(), bytes.end(), entry->data->data); board.update(&ctx, address, entry);
        for (unsigned i = 0; i < 64; ++i) memory.backend().access(address + i) = bytes[i];
    }
}
static void readCase(const std::filesystem::path& directory, unsigned form, unsigned retry, bool tag, bool fungible = false) {
    Context ctx; GlobalBoard<paddr_t> board; chi_agent::HomeMemory memory(ctx.cfg, ctx.cycles);
    EbHome home(memory, ctx, board, 8, 4); initialize(ctx, board, memory);
    CanonicalTrace trace(directory / "canonical", 42);
    struct Actor { EbRequest req; HomePlan plan; unsigned mask = 0, dbid = 0; bool retried = false, reissued = false, rsp = false, ack = false; };
    std::array<Actor, 2> actors{};
    for (unsigned i = 0; i < 2; ++i) {
        actors[i].req = {1, 8, 70+i, i ? 38U : 7U, 0x1000+i*64U, i ? !tag : tag};
        auto& plan = actors[i].plan;
        plan.separated = form != 0; plan.completionAfterData = form == 3;
        plan.completionSource = 14; plan.upperBeatFirst = i != 0;
        plan.responseTraceTag = tag; plan.retryOnce = retry != 0;
        plan.creditBeforeRetry = retry == 1;
        plan.retryIssuer = fungible ? 8 : i ? 14 : 8;
        plan.creditType = fungible ? 3 : i ? 4 : 3;
    }
    auto tick = [&] { memory.tick(); home.service(); ++ctx.cycles; };
    request(trace, ctx, actors[0].req); home.accept(actors[0].req, actors[0].plan); tick();
    request(trace, ctx, actors[1].req); home.accept(actors[1].req, actors[1].plan);
    std::map<std::pair<unsigned,unsigned>,unsigned> tokens;
    for (unsigned steps = 0; steps < 200 && !home.idle(); ++steps) {
        if (steps != 0) {
            const std::array<unsigned,2> order = fungible ? std::array<unsigned,2>{1,0} : std::array<unsigned,2>{0,1};
            for (unsigned i : order) {
                auto& a = actors[i]; auto& count = tokens[{a.plan.retryIssuer,a.plan.creditType}];
                if (a.retried && !a.reissued && count && (!fungible || (i ? actors[0].retried : actors[1].ack))) {
                    auto reissue = a.req; reissue.allowRetry = false; reissue.pCrdType = a.plan.creditType;
                    request(trace, ctx, reissue); home.accept(reissue); --count; a.reissued = true;
                    break; // One physical REQ per cycle.
                }
            }
        }
        if (home.responseValid()) {
            const auto r = home.response(); bool ready = ctx.cycles % 4 != 0;
            if (r.opcode == 11 && form == 2) ready = ready && actors.at(r.txn-70).mask != 0;
            if (fungible && r.opcode == 7 && actors[1].retried && !actors[1].ack) ready = false;
            response(trace, ctx, Channel::ChiRxRsp, r, ready);
            if (ready) {
                if (r.opcode == 3) actors.at(r.txn-70).retried = true;
                else if (r.opcode == 7) ++tokens[{r.source,r.pCrdType}];
                else actors.at(r.txn-70).rsp = true;
                home.responseAccepted();
            }
        }
        if (home.dataValid()) {
            const auto r = home.datum(); const bool ready = ctx.cycles % 3 != 0;
            auto& a = actors.at(r.txn-70);
            const auto golden = initial(a.req.address);
            require(std::equal(r.data.begin(), r.data.end(), golden.begin()+r.dataId/2*32), "matrix Home data differs from independent initialization");
            data(trace, ctx, Channel::ChiRxDat, r, ready);
            if (ready) { a.mask |= 1U << (r.dataId/2); a.dbid = r.dbid; home.dataAccepted(); }
        }
        // One CompAck per cycle, after both accepted completion halves.
        for (auto& a : actors) if (!a.ack && a.mask == 3 && (!a.plan.separated || a.rsp)) {
            const bool ready = ctx.cycles % 5 != 0;
            const unsigned target = a.plan.separated ? 14 : 8;
            const bool ackTag = a.plan.separated ? a.plan.responseTraceTag : a.req.traceTag || a.plan.responseTraceTag;
            response(trace, ctx, Channel::ChiTxRsp, {1,target,a.dbid,2,0,0,ackTag}, ready);
            if (ready) { home.receiveCompAck(1,target,a.dbid,ackTag); a.ack = true; }
            break;
        }
        tick();
    }
    require(home.idle() && actors[0].ack && actors[1].ack, "matrix read/retry contexts did not drain");
    require(std::all_of(tokens.begin(),tokens.end(),[](const auto& pair) { return pair.second==0; }), "matrix driver left credits");
}
static void roleCase(const std::filesystem::path& directory) {
    Context ctx; GlobalBoard<paddr_t> board; chi_agent::HomeMemory memory(ctx.cfg, ctx.cycles);
    EbHome home(memory, ctx, board, 8, 4); initialize(ctx, board, memory);
    CanonicalTrace trace(directory / "canonical", 42);
    auto tick = [&] { memory.tick(); home.service(); ++ctx.cycles; };
    EbRequest acquire{1,8,70,38,0x1000,true}; request(trace,ctx,acquire); home.accept(acquire);
    for (unsigned i=0;i<80 && !home.dataValid();++i) tick();
    require(home.dataValid(),"role matrix acquisition stalled");
    const unsigned readDbid=home.datum().dbid;
    for (unsigned i=0;i<2;++i) { data(trace,ctx,Channel::ChiRxDat,home.datum(),true); home.dataAccepted(); tick(); }
    EbRequest victim{1,8,70,66,0x1040,false}; victim.likelyShared=true;
    request(trace,ctx,victim); home.accept(victim); tick();
    const auto comp=home.response(); const unsigned writeDbid=comp.dbid;
    require(readDbid!=writeDbid,"read and victim DBID unexpectedly alias");
    response(trace,ctx,Channel::ChiRxRsp,comp,true); home.responseAccepted(); tick();
    bool rejected=false;
    try { home.receiveCompAck(1,8,writeDbid,false); } catch (const std::runtime_error&) { rejected=true; }
    require(rejected,"Home accepted victim Ack before acquisition Ack");
    response(trace,ctx,Channel::ChiTxRsp,{1,8,readDbid,2,0,0,true},true); home.receiveCompAck(1,8,readDbid,true); tick();
    response(trace,ctx,Channel::ChiTxRsp,{1,8,writeDbid,2,0,0,false},true); home.receiveCompAck(1,8,writeDbid,false);
    require(home.idle(),"role matrix leaked a context");
}
static void permissionCase(const std::filesystem::path& directory) {
    Context ctx; GlobalBoard<paddr_t> board; chi_agent::HomeMemory memory(ctx.cfg, ctx.cycles);
    EbHome home(memory,ctx,board,8,4); initialize(ctx,board,memory);
    CanonicalTrace trace(directory / "canonical",42);
    EbRequest req{1,8,70,12,0x1000,true}; request(trace,ctx,req); home.accept(req); ++ctx.cycles;
    const auto comp=home.response();
    for (unsigned i=0;i<3;++i) { response(trace,ctx,Channel::ChiRxRsp,comp,false); ++ctx.cycles; }
    response(trace,ctx,Channel::ChiRxRsp,comp,true); home.responseAccepted(); ++ctx.cycles;
    response(trace,ctx,Channel::ChiTxRsp,{1,comp.source,comp.dbid,2,0,0,comp.traceTag},true);
    home.receiveCompAck(1,comp.source,comp.dbid,comp.traceTag); require(home.idle(),"permission matrix leaked");
}
static void upstreamCase(const std::filesystem::path& directory, bool combined, bool simultaneous) {
    Context ctx; ArchitecturalBoard board(ctx,8); Requester node(board,1);
    const auto old=initial(0x1000); auto latest=old; latest[0]^=0xa5;
    board.initialize(0x1000,old); node.enqueue({0,1,1,16,0x1000,true}); node.requestAccepted();
    for (unsigned beat=0;beat<2;++beat) {
        DataFlit r{1,0,1,10,beat,2,true}; std::copy_n(old.begin()+beat*32,32,r.data.begin()); node.receiveData(r);
    }
    node.responseAccepted(); node.store(0,0x1000,latest);
    node.enqueueAction(0,{TestActionKind::WriteBack,0x1000,23,true});
    CanonicalTrace trace(directory / "canonical",42);
    auto evt=event(ctx,Channel::CchiRxEvt,0,1,23,1,true,true); evt.address=0x1000;
    trace.record(evt); node.eventAccepted(); ++ctx.cycles;
    auto grant=event(ctx,Channel::CchiTxRsp,1,0,23,combined ? 3 : 2,false,true);
    grant.dbid=16; grant.resp=0; grant.respErr=0;
    for (unsigned i=0;i<3;++i) { grant.cycle=ctx.cycles; grant.simTime=ctx.cycles*2; trace.record(grant); require(!node.dataValid(),"WriteBack DAT preceded DBID acceptance"); ++ctx.cycles; }
    grant.cycle=ctx.cycles; grant.simTime=ctx.cycles*2; grant.ready=true; trace.record(grant);
    node.receiveResponse({1,0,23,combined ? 3U : 2U,0,true},16,0); ++ctx.cycles;
    unsigned beats=0;
    while (node.dataValid()) {
        const auto r=node.data(); const bool ready=ctx.cycles%3!=0;
        auto e=event(ctx,Channel::CchiRxDat,r.node,r.target,r.txn,r.opcode,ready,r.traceTag);
        e.dataId=r.dataId; e.resp=r.resp; e.respErr=r.respErr; e.byteEnable=r.byteEnable; e.data=r.data;
        require(std::equal(r.data.begin(),r.data.end(),latest.begin()+r.dataId*32),"WriteBack changed independent Store bytes");
        const bool last=ready && beats==1;
        if (!combined && simultaneous && last) {
            auto comp=event(ctx,Channel::CchiTxRsp,1,0,23,1,true,true); comp.resp=0; comp.respErr=0;
            trace.record(comp); // Native channel print order: Comp before same-cycle final DAT.
        }
        trace.record(e);
        if (ready) { node.dataAccepted(); ++beats; }
        if (!combined && simultaneous && last) node.receiveResponse({1,0,23,1,0,true},0,0);
        ++ctx.cycles;
    }
    if (!combined && !simultaneous) {
        require(!node.idle(),"split WriteBack retired before Comp");
        auto comp=event(ctx,Channel::CchiTxRsp,1,0,23,1,false,true); comp.resp=0; comp.respErr=0;
        for (unsigned i=0;i<3;++i) { comp.cycle=ctx.cycles; comp.simTime=ctx.cycles*2; trace.record(comp); ++ctx.cycles; }
        comp.cycle=ctx.cycles; comp.simTime=ctx.cycles*2; comp.ready=true; trace.record(comp);
        node.receiveResponse({1,0,23,1,0,true},0,0);
    }
    require(beats==2 && node.idle() && board.idle(),"upstream WriteBack obligations did not drain");
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected matrix output directory"); const std::filesystem::path root(argv[1]);
        unsigned count=0;
        for (unsigned form=0;form<4;++form) for(unsigned retry=0;retry<3;++retry) for(bool tag:{false,true}) {
            const auto label="read-"+std::to_string(form)+"-retry-"+std::to_string(retry)+"-tag-"+std::to_string(tag);
            readCase(root/label,form,retry,tag); ++count;
        }
        readCase(root/"fungible-outliving-credit",0,2,false,true); ++count;
        roleCase(root/"reused-acquire-victim-txn"); ++count;
        permissionCase(root/"dataless-comp"); ++count;
        for (const auto [combined,simultaneous] : {std::pair{true,false},std::pair{false,false},std::pair{false,true}}) {
            upstreamCase(root/("upstream-"+std::to_string(combined)+"-same-cycle-"+std::to_string(simultaneous)),combined,simultaneous); ++count;
        }
        std::printf("Transport endpoints: %u accepted matrix cases generated from existing Home/Requester\n",count);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
