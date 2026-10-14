#include "EbHome.hpp"
#include <algorithm>
#include <stdexcept>

namespace compactchi {
void EbHome::require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool EbHome::isRead(unsigned op) { return op == 7 || op == 0x26 || op == 4; }
bool EbHome::isWrite(unsigned op) { return op == 0x42 || op == 0x1b || op == 0x17 || op == 0x0d || op == 0x1c || op == 0x1d; }
EbHome::EbHome(chi_agent::HomeMemory& memory, TLLocalContext& local, GlobalBoard<paddr_t>& global,
               unsigned node, size_t capacity)
    : memory(memory), local(local), global(global), node(node), capacity(capacity) {
    require(capacity > 0 && capacity <= 4096 && node < 2048, "invalid E.b Home capacity or node identity");
}
bool EbHome::requestReady(const EbRequest& req) const {
    const auto key = std::make_tuple(req.source, req.txn, isWrite(req.opcode));
    const auto previous = transactions.find(key);
    const bool reissue = previous != transactions.end() && contexts.at(previous->second).retryPending;
    if (!reissue && contexts.size() >= capacity) return false;
    if (req.opcode == 4 || req.opcode == 0x1c || req.opcode == 0x1d) return true;
    if (req.opcode == 0x10 || req.opcode == 0x11 || req.opcode == 0x0f)
        for (const auto& [id, c] : contexts) if (c.copyback && c.request.address == req.address) return false;
    if (!isRead(req.opcode)) return true; // Copyback storage is reserved at DBID allocation.
    unsigned reservedReads = 0;
    for (const auto& [id, ctx] : contexts) {
        if (ctx.copyback && ctx.request.address == req.address) return false;
        if (!ctx.write && (ctx.memoryPending || ctx.retryPending)) ++reservedReads;
    }
    const auto depth = memory.backend().config().memoryReadDepth;
    if (!reissue && depth && reservedReads >= depth) return false;
    return memory.ready(false);
}
void EbHome::accept(const EbRequest& req, const HomePlan& plan) {
    const bool device = req.opcode == 4 || req.opcode == 0x1c || req.opcode == 0x1d;
    const bool control = req.opcode == 0x0c || req.opcode == 0x10 || req.opcode == 0x11 || req.opcode == 0x0f;
    require(isRead(req.opcode) || isWrite(req.opcode) || control, "unsupported E.b Home opcode");
    require(req.target == node && req.source < 2048 && req.txn < 4096 &&
            (device ? req.size <= 6 && !(req.address & ((uint64_t(1) << req.size) - 1)) : !(req.address & 63) && req.size == 6),
            "invalid E.b route, size or TxnID");
    if (device) {
        const auto& cfg = local.config();
        require(cfg.mmioEnable && req.address >= cfg.mmioStart && req.address < cfg.mmioEnd &&
            (uint64_t(1) << req.size) <= cfg.mmioEnd - req.address && !req.allowRetry, "device outside configured no-retry region");
    } else {
        require((req.memAttr & 4) && req.order == 0 && !req.ns, "unsupported cacheable attributes");
        require(memory.backend().accessible(req.address) && memory.backend().accessible(req.address + 63), "Home line outside memory");
        if (control && req.opcode != 0x0c) require(local.config().cmoEnable, "CMO disabled by configuration");
    }
    require(requestReady(req), "E.b Home accepted without reserved capacity");
    const bool write = isWrite(req.opcode);
    const auto key = std::make_tuple(req.source, req.txn, write);
    const auto previous = transactions.find(key);
    if (previous != transactions.end()) {
        auto& ctx = contexts.at(previous->second);
        require(ctx.retryPending && ctx.retryDelivered && !req.allowRetry && req.pCrdType == ctx.plan.creditType,
                "unexpected reissue or Retry repeated without the required credit type");
        const auto& original = ctx.request;
        require(req.address == original.address && req.opcode == original.opcode && req.size == original.size &&
                req.source == original.source && req.target == original.target && req.traceTag == original.traceTag &&
                req.expCompAck == original.expCompAck && req.memAttr == original.memAttr &&
                req.order == original.order && req.ns == original.ns && req.likelyShared == original.likelyShared,
                "reissue changed its original transaction attributes");
        const unsigned issuer = ctx.plan.retryIssuer ? ctx.plan.retryIssuer : node;
        const auto creditKey = std::make_pair(issuer, ctx.plan.creditType);
        const auto credit = credits.find(creditKey);
        require(credit != credits.end() && credit->second != 0, "reissue consumed a missing issuer/type P-Credit");
        if (--credit->second == 0) credits.erase(credit);
        ctx.retryPending = false;
        start(previous->second);
        return;
    }
    require((device ? !req.allowRetry : req.allowRetry) && req.pCrdType == 0, "initial retry/credit contract invalid");
    if (!write) require(req.expCompAck == (!device && (!control || req.opcode == 0x0c)), "CompAck contract invalid");
    if (req.opcode == 0x42) require(req.expCompAck, "WriteEvictOrEvict did not request CompAck");
    if (write && req.opcode != 0x42) require(!req.expCompAck, "ordinary copyback/Evict must not request CompAck");
    require(plan.completionSource < 2048 && plan.retryIssuer < 2048 && plan.creditType < 16,
            "E.b response source or credit type is out of range");
    unsigned dbid = nextDbid;
    while (dbids.count(dbid)) dbid = (dbid + 1) % 4096;
    nextDbid = (dbid + 1) % 4096;
    const unsigned owner = nextOwner++;
    require(!contexts.count(owner), "Home internal memory ID wrapped into a live context");
    const bool copyback = req.opcode == 0x1b || req.opcode == 0x17 ||
        (req.opcode == 0x42 && plan.cleanCopyback);
    const bool ack = (!write && req.expCompAck) || (req.opcode == 0x42 && !copyback);
    const unsigned ackTarget = !write && !control && !device && plan.separated && plan.completionSource ?
        plan.completionSource : node;
    Context ctx{req, plan, dbid, ackTarget, write, copyback, ack};
    ctx.control = control; ctx.device = device;
    if (device) {
        ctx.dataMask = req.size == 6 ? 3 : 1U << ((req.address >> 5) & 1);
        if (write) {
            require(bool(deviceOracle), "device write lacks independent node intent");
            const auto expected = deviceOracle(req.address); ctx.expected = expected.first; ctx.byteMask = expected.second;
        }
    }
    if (copyback) {
        // This is expected architectural data, never data learned from the DUT.
        if (global.haskey(req.address)) {
            const auto entry = global.query(&local, req.address);
            require(entry->status == Global_SBEntry::SB_VALID && entry->data,
                    "copyback needs a committed independent expected line");
            std::copy_n(entry->data->data, 64, ctx.expected.begin());
        }
    }
    contexts.emplace(owner, ctx);
    transactions.emplace(key, owner);
    dbids.emplace(dbid, owner);
    if (plan.retryOnce && !device) {
        contexts.at(owner).retryPending = true;
        const unsigned issuer = plan.retryIssuer ? plan.retryIssuer : node;
        const QueuedRsp retry{{issuer, req.source, req.txn, 3, 0, 0, false, plan.creditType}, owner, RspKind::Retry};
        const QueuedRsp credit{{issuer, req.source, 0, 7, 0, 0, false, plan.creditType}, owner, RspKind::Credit};
        responses.push_back(plan.creditBeforeRetry ? credit : retry);
        responses.push_back(plan.creditBeforeRetry ? retry : credit);
    } else start(owner);
}
void EbHome::start(unsigned owner) {
    auto& ctx = contexts.at(owner);
    const auto& req = ctx.request;
    const auto& plan = ctx.plan;
    if (ctx.device) {
        const bool responseTraceTag = req.traceTag;
        if (ctx.write) responses.push_back({{node, req.source, req.txn, 5, ctx.dbid, 0, responseTraceTag}, owner});
        else {
            const auto& bytes = deviceMemory[req.address & ~uint64_t(63)];
            for (unsigned beat = 0; beat < 2; ++beat) if (ctx.dataMask & (1U << beat)) {
                EbData flit{node, req.source, req.txn, 4, ctx.dbid, node, beat * 2, 0, responseTraceTag};
                std::copy_n(bytes.begin() + beat * 32, 32, flit.data.begin()); data.push_back({flit, owner});
            }
        }
    } else if (ctx.control) {
        responses.push_back({{node, req.source, req.txn, 4, ctx.dbid, req.opcode == 0x0c ? 2U : 0U, plan.responseTraceTag}, owner});
    } else if (!ctx.write) {
        memory.submit({owner, req.address, false});
        ctx.memoryPending = true;
        if (plan.separated && !plan.completionAfterData) enqueueCompletion(owner);
    } else {
        const unsigned opcode = ctx.copyback ? 5 : 4; // CompDBIDResp versus Comp
        responses.push_back({{node, req.source, req.txn, opcode, ctx.dbid, 2, plan.responseTraceTag}, owner});
    }
}
void EbHome::enqueueCompletion(unsigned owner) {
    const auto& ctx = contexts.at(owner);
    responses.push_back({{ctx.ackTarget, ctx.request.source, ctx.request.txn, 0x0b,
                          ctx.dbid, ctx.request.opcode == 7 ? 2U : 1U, ctx.plan.responseTraceTag}, owner});
}
void EbHome::finish(unsigned owner) {
    const auto& ctx = contexts.at(owner);
    if (!ctx.homeMemoryOnly) {
        transactions.erase({ctx.request.source, ctx.request.txn, ctx.write});
        dbids.erase(ctx.dbid);
    }
    contexts.erase(owner);
}
void EbHome::service() {
    // Incoming copyback data has reserved storage even when the original
    // memory backend is busy. Submit complete lines when its capacity permits.
    for (auto& [owner, ctx] : contexts) {
        if (ctx.copyback && ctx.receivedMask == 3 && !ctx.memoryPending && memory.ready(true)) {
            memory.submit({owner, ctx.request.address, true, ctx.received});
            ctx.memoryPending = true;
        }
    }
    while (memory.completionValid()) {
        const auto result = memory.completion();
        auto it = contexts.find(result.id);
        require(it != contexts.end(), "memory completion has no E.b Home owner");
        auto& ctx = it->second;
        require(result.address == ctx.request.address && ctx.memoryPending,
                "memory completion changed the Home line/owner");
        if (result.write) {
            require(ctx.copyback && ctx.receivedMask == 3, "write memory completion preceded complete copyback");
            if (committed) committed(ctx.request.address, ctx.expected);
            memory.popCompletion();
            finish(result.id);
        } else {
            require(!ctx.write, "read memory completion targets a write context");
            const unsigned first = ctx.plan.upperBeatFirst ? 1 : 0;
            for (unsigned i = 0; i < 2; ++i) {
                const unsigned beat = i ? 1 - first : first;
                EbData flit{node, ctx.request.source, ctx.request.txn,
                    ctx.plan.separated ? 0xbU : 4U, ctx.dbid, node, beat * 2,
                    ctx.request.opcode == 7 ? 2U : 1U, ctx.plan.responseTraceTag};
                std::copy_n(result.data.begin() + beat * 32, 32, flit.data.begin());
                data.push_back({flit, result.id});
            }
            ctx.memoryPending = false;
            memory.popCompletion();
        }
    }
}
void EbHome::responseAccepted() {
    require(responseValid(), "Home consumed an absent E.b response");
    const auto item = responses.front();
    if (item.kind == RspKind::Credit) {
        // E.b credits are fungible by issuer/type, not tied to a TxnID. An
        // early credit can also outlive the transaction whose retry queued it.
        ++credits[{item.flit.source, item.flit.pCrdType}];
        responses.pop_front();
        return;
    }
    auto& ctx = contexts.at(item.owner);
    if (item.kind == RspKind::Retry) {
        require(ctx.retryPending && !ctx.retryDelivered, "RetryAck delivered twice");
        ctx.retryDelivered = true;
        responses.pop_front();
        return;
    }
    require(!ctx.rspAccepted, "Home completion delivered twice");
    ctx.rspAccepted = true;
    const bool retire = (ctx.write || ctx.control) && !ctx.copyback && !ctx.needAck && !ctx.device;
    responses.pop_front();
    if (retire) finish(item.owner);
}
void EbHome::dataAccepted() {
    require(dataValid(), "Home consumed absent E.b data");
    const auto item = data.front();
    auto& ctx = contexts.at(item.owner);
    const unsigned beat = item.flit.dataId / 2;
    require(!(ctx.sentMask & (1U << beat)), "Home read data delivered twice");
    ctx.sentMask |= 1U << beat;
    if (ctx.sentMask == 3 && ctx.plan.separated && ctx.plan.completionAfterData) enqueueCompletion(item.owner);
    const bool deviceDone = ctx.device && ctx.sentMask == ctx.dataMask;
    data.pop_front();
    if (deviceDone) finish(item.owner);
}
void EbHome::receiveCompAck(unsigned source, unsigned target, unsigned dbid, bool traceTag) {
    require(dbids.count(dbid), "unknown E.b CompAck DBID");
    const unsigned owner = dbids.at(dbid);
    const auto& ctx = contexts.at(owner);
    require(!ctx.retryPending && ctx.needAck && source == ctx.request.source && target == ctx.ackTarget,
            "unexpected or misrouted E.b CompAck");
    if (ctx.control) {
        require(ctx.rspAccepted && traceTag == ctx.plan.responseTraceTag, "permission CompAck completion/tag mismatch");
    } else if (!ctx.write) {
        require(ctx.sentMask == 3 && (!ctx.plan.separated || ctx.rspAccepted),
                "AnserL2 CompAck preceded full read completion");
        const bool expectedTag = ctx.plan.separated ? ctx.plan.responseTraceTag :
            ctx.request.traceTag || ctx.plan.responseTraceTag;
        require(traceTag == expectedTag, "read CompAck TraceTag differs from the CPL2 completion context");
    } else {
        require(ctx.rspAccepted, "write CompAck preceded Comp");
        require(!transactions.count({ctx.request.source, ctx.request.txn, false}),
                "CPL2 victim CompAck preceded its live acquisition CompAck");
        require(traceTag == ctx.plan.responseTraceTag, "write CompAck did not use Comp TraceTag");
    }
    finish(owner);
}
void EbHome::receiveCopyback(const EbData& flit) {
    require(dbids.count(flit.txn), "unknown copyback DBID");
    auto& ctx = contexts.at(dbids.at(flit.txn));
    require(ctx.copyback && ctx.rspAccepted && flit.source == ctx.request.source && flit.target == node,
            "copyback preceded DBID response or used an incorrect route");
    require(flit.opcode == 2 && flit.respErr == 0 && (flit.dataId == 0 || flit.dataId == 2),
            "invalid normal-profile CopyBackWrData opcode/error/beat");
    require(flit.traceTag == ctx.plan.responseTraceTag, "copyback did not use CompDBIDResp TraceTag");
    require(flit.byteEnable == 0xffffffffU && (flit.resp == 1 || flit.resp == 2 || flit.resp == 5 || flit.resp == 6),
            "valid copyback requires complete bytes and legal permission");
    if (ctx.request.opcode != 0x42) require((flit.resp & 4) != 0, "dirty release lost PassDirty responsibility");
    const unsigned beat = flit.dataId / 2;
    require(!(ctx.receivedMask & (1U << beat)), "duplicate copyback data beat");
    require(std::equal(flit.data.begin(), flit.data.end(), ctx.expected.begin() + beat * 32),
            "copyback differs from the independent architectural data");
    std::copy(flit.data.begin(), flit.data.end(), ctx.received.begin() + beat * 32);
    ctx.receivedMask |= 1U << beat;
}
void EbHome::commitSnoopData(uint64_t address, const chi_agent::Line& data) {
    require(snoopCommitReady() && !(address & 63) && global.haskey(address),
            "Snoop dirty commitment lacks memory capacity or expected data");
    const auto golden = global.query(&local, address);
    require(golden->status == Global_SBEntry::SB_VALID && golden->data &&
            std::equal(data.begin(), data.end(), golden->data->data),
            "Snoop dirty commitment differs from independent architectural data");
    const unsigned owner = nextOwner++;
    require(!contexts.count(owner), "Snoop memory commitment reused a live owner");
    // A Home-owned memory write, not a fabricated DUT CHI request. Its memory
    // completion is retained by this context until the backend actually commits.
    EbRequest request{node, node, owner, 0x17, address, false, false};
    Context ctx{request, {}, 0, node, true, true, false};
    ctx.rspAccepted = true;
    ctx.homeMemoryOnly = true;
    ctx.receivedMask = 3;
    std::copy_n(golden->data->data, 64, ctx.expected.begin());
    ctx.received = data;
    ctx.memoryPending = true;
    contexts.emplace(owner, ctx);
    memory.submit({owner, address, true, data});
}
bool EbHome::idle() const {
    return contexts.empty() && transactions.empty() && dbids.empty() && credits.empty() &&
           responses.empty() && data.empty() && memory.idle();
}
} // namespace compactchi
