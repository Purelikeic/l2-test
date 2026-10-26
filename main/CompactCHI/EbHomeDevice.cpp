#include "EbHome.hpp"

namespace compactchi {
void EbHome::receiveDeviceData(const EbData& flit) {
    require(dbids.count(flit.txn), "unknown device write DBID");
    const auto owner = dbids.at(flit.txn);
    auto& ctx = contexts.at(owner);
    require(ctx.device && ctx.write && ctx.rspAccepted && flit.source == ctx.request.source && flit.target == node,
        "device data preceded DBID or route invalid");
    require(flit.opcode == 3 && flit.respErr == 0 && (flit.dataId == 0 || flit.dataId == 2) &&
        flit.traceTag == ctx.request.traceTag, "device write data opcode/tag/error invalid");
    const unsigned beat = flit.dataId / 2;
    require((ctx.dataMask & (1U << beat)) && !(ctx.receivedMask & (1U << beat)), "device data beat invalid/duplicate");
    require(flit.byteEnable == uint32_t(ctx.byteMask >> (beat * 32)), "device byte enables differ from independent intent");
    auto& actual = deviceMemory[ctx.request.address & ~uint64_t(63)];
    for (unsigned i = 0; i < 32; ++i) if (flit.byteEnable & (uint32_t(1) << i)) {
        require(flit.data[i] == ctx.expected[beat * 32 + i], "device write differs from independent action data");
        actual[beat * 32 + i] = flit.data[i];
    }
    ctx.receivedMask |= 1U << beat;
    if (ctx.receivedMask != ctx.dataMask) return;
    if (!ctx.ordered) { finish(owner); return; }
    if (!ctx.compQueued) enqueueOrderedComp(owner); // Comp after data
    else if (ctx.compAccepted) finish(owner);
}
} // namespace compactchi
