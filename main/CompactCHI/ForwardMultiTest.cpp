#include "ForwardNodeBoard.hpp"
#include "LowerSnoopBoard.hpp"
#include "CanonicalTrace.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace compactchi;
class Context : public TLLocalContext {
    TLLocalConfig cfg{};
public:
    uint64_t cycle() const noexcept override { return 0; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return 42; }
    const TLLocalConfig& config() const noexcept override { return cfg; }
    TLLocalConfig& config() noexcept override { return cfg; }
};
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static unsigned rejected=0;
static void reject(const std::function<void()>& action) {
    bool failed=false; try { action(); } catch(const std::runtime_error&) { failed=true; }
    require(failed,"multi-forward endpoint accepted an invalid input"); ++rejected;
}
static std::string hex(const LineData& data) {
    const char* digits="0123456789abcdef"; std::string result;
    for(auto b:data) { result+=digits[b>>4]; result+=digits[b&15]; } return result;
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected multi-forward output directory");
        const std::filesystem::path root(argv[1]); std::filesystem::create_directories(root);
        Context ctx; GlobalBoard<paddr_t> global; LowerSnoopBoard home(ctx,global); ForwardNodeBoard forward;
        CanonicalTrace trace(root/"canonical",42); std::ofstream intents(root/"intents.jsonl");
        std::array<LineData,3> versions{};
        for(unsigned line=0;line<3;++line) {
            const uint64_t address=0x1000+line*64;
            for(unsigned i=0;i<64;++i) versions[line][i]=uint8_t((address>>6)^(i*17)^42);
            if(line==1) versions[line][0]^=0xa5; // Explicit independently intended dirty version.
            auto entry=std::make_shared<Global_SBEntry>(); entry->status=Global_SBEntry::SB_VALID;
            entry->data=make_shared_tldata<64>(); std::copy(versions[line].begin(),versions[line].end(),entry->data->data);
            global.update(&ctx,address,entry);
        }
        std::array<LowerSnoop,3> snoops{{{50,23,0x1000,false,true,12,500},
                                       {51,23,0x1040,false,false,12,501},
                                       {50,23,0x1080,false,true,12,500}}};
        auto issue=[&](unsigned line,uint64_t cycle) {
            const auto& s=snoops[line]; const auto before=line==1 ? Permission::UD : Permission::UC;
            home.issue(s,before);
            CanonicalEvent e{cycle,cycle*2,Channel::ChiRxSnp,8,1,s.txn,s.opcode,true,true,s.traceTag};
            e.address=s.address; e.forwardNode=s.forwardNode; e.forwardTxn=s.forwardTxn; trace.record(e);
            intents << "{\"node\":8,\"target\":1,\"txn_id\":" << s.txn << ",\"opcode\":23,\"address\":" << s.address
                    << ",\"trace_tag\":" << (s.traceTag ? "true" : "false") << ",\"forward_node\":12,\"forward_txn\":" << s.forwardTxn
                    << ",\"before\":" << unsigned(before) << ",\"data\":\"" << hex(versions[line]) << "\"}\n";
        };
        auto beat=[&](unsigned line,unsigned id) {
            const auto& s=snoops[line]; EbData d{1,12,s.forwardTxn,4,s.txn,8,id,line==1 ? 6U : 2U,s.traceTag};
            std::copy_n(versions[line].begin()+id/2*32,32,d.data.begin()); return d;
        };
        auto accept=[&](unsigned line,unsigned id,uint64_t cycle) {
            const auto d=beat(line,id); home.forwardData(d); forward.accept(d,cycle);
            CanonicalEvent e{cycle,cycle*2,Channel::ChiTxDat,d.source,d.target,d.txn,d.opcode,true,true,d.traceTag};
            e.dbid=d.dbid; e.homeNode=d.home; e.dataId=d.dataId; e.resp=d.resp; e.respErr=d.respErr;
            e.byteEnable=d.byteEnable; e.data=d.data; trace.record(e);
        };
        auto response=[&](unsigned line,uint64_t cycle) {
            const auto& s=snoops[line]; EbResponse r{1,8,s.txn,9,0,0,s.traceTag}; r.fwdState=line==1 ? 6 : 2;
            home.response(r);
            CanonicalEvent e{cycle,cycle*2,Channel::ChiTxRsp,1,8,s.txn,9,true,true,s.traceTag};
            e.resp=0; e.respErr=0; e.forwardState=r.fwdState; trace.record(e);
        };
        for(uint64_t cycle=0;cycle<=24;++cycle) {
            if(auto ack=forward.tick(cycle)) {
                require(ack->source==12 && ack->target==8 && ack->opcode==2,"external Ack route changed");
                home.forwardCompAck(ack->source,ack->target,ack->txn,ack->traceTag);
                if(cycle==14) { require(home.complete(50),"A lacked Home completion"); home.retire(50); }
                if(cycle==15) require(!home.complete(51),"B retired before delayed Home completion");
                if(cycle==23) { require(home.complete(50),"reused C context incomplete"); home.retire(50); }
                // The Ack is host-to-host: it is never a canonical DUT event.
            }
            if(cycle==1) issue(0,cycle);
            if(cycle==2) {
                issue(1,cycle);
                reject([&]{ auto bad=snoops[2]; bad.txn=52; bad.forwardTxn=502; bad.address=0x1000; home.issue(bad,Permission::UC); });
            }
            if(cycle==4) response(0,cycle);
            if(cycle==10) {
                auto good=beat(0,0);
                reject([&]{ auto bad=good; bad.source=2; forward.accept(bad,cycle); });
                reject([&]{ auto bad=good; bad.dataId=1; forward.accept(bad,cycle); });
                reject([&]{ auto bad=good; bad.home=9; forward.accept(bad,cycle); });
                accept(0,0,cycle);
                reject([&]{ forward.accept(good,cycle); });
                reject([&]{ auto bad=beat(1,0); bad.dbid=50; forward.accept(bad,cycle); });
                reject([&]{ auto bad=beat(1,0); bad.txn=500; forward.accept(bad,cycle); });
                reject([&]{ home.forwardCompAck(12,8,50,true); });
            }
            if(cycle==11) accept(1,0,cycle);
            if(cycle==13) accept(0,2,cycle);
            if(cycle==14) accept(1,2,cycle);
            if(cycle==17) {
                reject([&]{ auto bad=snoops[2]; bad.txn=51; bad.forwardTxn=502; home.issue(bad,Permission::UC); });
                issue(2,cycle); // A's Home/Fwd identities are free, B still awaits Home RSP.
            }
            if(cycle==18) { response(1,cycle); require(home.complete(51),"B did not join Home/Ack"); home.retire(51); }
            if(cycle==19) accept(2,0,cycle);
            if(cycle==20) response(2,cycle);
            if(cycle==22) accept(2,2,cycle);
        }
        require(home.idle() && forward.idle(),"multi-forward obligations leaked");
        std::ofstream out(root/"observations.jsonl");
        for(const auto& r:forward.observations())
            out << "{\"kind\":\"" << r.kind << "\",\"cycle\":" << r.cycle << ",\"txn\":" << r.txn << ",\"dbid\":" << r.dbid
                << ",\"source\":" << r.source << ",\"target\":" << r.target << ",\"beat\":" << r.beat
                << ",\"trace_tag\":" << (r.traceTag ? "true" : "false") << "}\n";
        require(intents.good() && out.good(),"multi-forward evidence write failed");
        std::printf("Multi-forward endpoints: three obligations, two concurrently live, delayed Home join and identity reuse; %u invalid inputs rejected\n",rejected);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
