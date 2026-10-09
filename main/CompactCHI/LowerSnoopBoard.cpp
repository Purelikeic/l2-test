#include "LowerSnoopBoard.hpp"
#include <algorithm>
#include <stdexcept>

namespace compactchi {
void LowerSnoopBoard::require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool LowerSnoopBoard::forwarded(unsigned op) { return op == 0x11 || op == 0x17; }
LowerSnoopBoard::LowerSnoopBoard(TLLocalContext& context, GlobalBoard<paddr_t>& global,
                               unsigned dut, unsigned home, size_t capacity)
    : context(context), global(global), dut(dut), home(home), capacity(capacity) {
    require(capacity && dut < 2048 && home < 2048, "invalid lower Snoop checker configuration");
}
void LowerSnoopBoard::issue(const LowerSnoop& req, Permission before) {
    require(req.opcode == 1 || req.opcode == 3 || req.opcode == 7 || req.opcode == 9 ||
            req.opcode == 10 || req.opcode == 16 || forwarded(req.opcode), "unsupported stable Snoop profile opcode");
    require(active.size() < capacity && !active.count(req.txn) && req.txn < 4096 && !(req.address & 63),
            "invalid or reused lower Snoop transaction");
    for (const auto& [id, entry] : active)
        require(entry.request.address != req.address, "stable-line Snoop profile must serialize a line");
    require(!(req.retToSrc && (req.opcode == 9 || req.opcode == 10 || req.opcode == 16 || req.opcode == 0x17)),
            "illegal RetToSrc for the selected Snoop opcode");
    const bool hit = before != Permission::I, dirty = before == Permission::UD;
    const bool shared = req.opcode == 1 || req.opcode == 0x11;
    const bool invalidate = req.opcode == 7 || req.opcode == 9 || req.opcode == 10 || req.opcode == 0x17;
    const bool discard = req.opcode == 10;
    const bool query = req.opcode == 16;
    const bool dct = forwarded(req.opcode) && hit;
    if (forwarded(req.opcode)) {
        require(req.forwardNode < 2048 && req.forwardNode != dut && req.forwardNode != home && req.forwardTxn < 4096,
                "invalid forward requester identity");
        require(!forward.count({req.forwardNode, req.forwardTxn}), "forward requester reused a live TxnID");
    }
    Permission after = before;
    if (invalidate) after = Permission::I;
    else if (shared && hit) after = Permission::SC;
    // Once returns a snapshot while retaining dirty responsibility. Query does
    // not return data. Neither operation converts a dirty resident line to clean.
    const bool returnData = hit && !discard && !query && req.opcode != 0x17 &&
        (dirty || (req.opcode == 3 && before == Permission::UC) ||
         (req.retToSrc && (forwarded(req.opcode) || before == Permission::SC)));
    const unsigned base = after == Permission::I ? 0 : after == Permission::SC ? 1 : 2;
    const bool passDirtyHome = returnData && dirty && req.opcode != 3;
    const unsigned homeResp = base | (passDirtyHome ? 4 : 0);
    const unsigned forwardResp = !dct ? 0 : req.opcode == 0x11 ? 1 : dirty ? 6 : 2;
    DirtySink sink = !dirty ? DirtySink::None : discard ? DirtySink::Discarded :
        req.opcode == 0x17 ? DirtySink::ForwardNode : passDirtyHome ? DirtySink::Home : DirtySink::Dut;
    SnoopResult result{after, sink, !dct ? Permission::I :
        req.opcode == 0x11 ? Permission::SC : dirty ? Permission::UD : Permission::UC, {}};
    if (hit) {
        require(global.haskey(req.address), "valid Snoop line has no independent expected data");
        const auto expected = global.query(&context, req.address);
        require(expected->status == Global_SBEntry::SB_VALID && expected->data,
                "Snoop needs independently committed latest data");
        std::copy_n(expected->data->data, 64, result.latest.begin());
    }
    active.emplace(req.txn, Entry{req, result, returnData, dct, false, false, homeResp, forwardResp});
    if (dct) forward.emplace(std::make_pair(req.forwardNode, req.forwardTxn), req.txn);
}
void LowerSnoopBoard::response(const EbResponse& rsp) {
    require(active.count(rsp.txn), "unknown lower Snoop response TxnID");
    auto& entry = active.at(rsp.txn);
    require(!entry.homeData && !entry.homeDone && rsp.source == dut && rsp.target == home &&
            rsp.opcode == (entry.dct ? 9U : 1U) && rsp.resp == entry.homeResp && rsp.respErr == 0 &&
            rsp.traceTag == entry.request.traceTag && rsp.fwdState == entry.forwardResp,
            "lower Snoop response changed state, routing, TraceTag or an obligation");
    entry.homeDone = true;
}
void LowerSnoopBoard::checkBytes(const Entry& entry, const EbData& data, unsigned mask) {
    require((data.dataId == 0 || data.dataId == 2) && !(mask & (1U << (data.dataId / 2))),
            "duplicate or invalid lower Snoop/DCT data beat");
    require(data.source == dut && data.respErr == 0 && data.byteEnable == 0xffffffffU &&
            data.traceTag == entry.request.traceTag,
            "lower Snoop/DCT data source, error or TraceTag mismatch");
    require(std::equal(data.data.begin(), data.data.end(), entry.result.latest.begin() + data.dataId / 2 * 32),
            "lower Snoop/DCT data differs from independent latest data");
}
void LowerSnoopBoard::homeData(const EbData& data) {
    require(active.count(data.txn), "unknown lower Snoop data TxnID");
    auto& entry = active.at(data.txn);
    require(entry.homeData && !entry.homeDone && data.target == home &&
            data.opcode == (entry.dct ? 6U : 1U) && data.resp == entry.homeResp && data.fwdState == entry.forwardResp,
            "unexpected Home Snoop data route, response state or opcode");
    checkBytes(entry, data, entry.homeMask);
    entry.homeMask |= 1U << (data.dataId / 2);
    entry.homeDone = entry.homeMask == 3;
}
void LowerSnoopBoard::forwardData(const EbData& data) {
    const auto key = std::make_pair(data.target, data.txn);
    require(forward.count(key), "unknown or misrouted DCT requester/TxnID");
    auto& entry = active.at(forward.at(key));
    require(entry.dct && !entry.forwardAck && data.opcode == 4 && data.home == home &&
            data.dbid == entry.request.txn && data.resp == entry.forwardResp,
            "DCT did not preserve HomeNID/DBID, permission or dirty transfer");
    checkBytes(entry, data, entry.forwardMask);
    entry.forwardMask |= 1U << (data.dataId / 2);
}
void LowerSnoopBoard::forwardCompAck(unsigned node, unsigned target, unsigned dbid, bool traceTag) {
    require(active.count(dbid), "unknown forwarded-read CompAck DBID");
    auto& entry = active.at(dbid);
    require(entry.dct && !entry.forwardAck && entry.forwardMask == 3 && node == entry.request.forwardNode &&
            target == home && traceTag == entry.request.traceTag, "invalid or premature forwarded-read CompAck");
    entry.forwardAck = true;
}
bool LowerSnoopBoard::complete(unsigned txn) const {
    const auto it = active.find(txn);
    return it != active.end() && it->second.homeDone && (!it->second.dct || it->second.forwardAck);
}
SnoopResult LowerSnoopBoard::retire(unsigned txn) {
    require(complete(txn), "lower Snoop retired before all Home/DCT obligations completed");
    const auto entry = active.at(txn);
    if (entry.dct) forward.erase({entry.request.forwardNode, entry.request.forwardTxn});
    active.erase(txn);
    return entry.result;
}
} // namespace compactchi
