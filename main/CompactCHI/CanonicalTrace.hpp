#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace compactchi {

// Names identify the DUT port direction; cycle is never derived from TraceTag.
enum class Channel {
    CchiRxReq, CchiRxEvt, CchiRxRsp, CchiRxDat, CchiTxSnp, CchiTxRsp, CchiTxDat,
    ChiTxReq, ChiRxRsp, ChiRxDat, ChiRxSnp, ChiTxRsp, ChiTxDat
};
struct CanonicalEvent {
    uint64_t cycle;
    std::optional<uint64_t> simTime;
    Channel channel;
    unsigned node, target, txn, opcode;
    bool valid, ready, traceTag;
    std::optional<uint64_t> address;
    std::optional<unsigned> dbid, dataId, resp, respErr;
    std::optional<unsigned> beforeState, afterState, owner, dataSource, prefetchSource;
    std::optional<bool> dirty;
    std::optional<std::array<uint8_t, 32>> data;
};

/** JSONL/text transport or semantic event sink, without any database dependency.
 * Producers must supply real cycle/handshake/correlation. Unknown fields stay
 * null; the sink does not infer state, routing, timing or architectural success. */
class CanonicalTrace {
    std::ofstream jsonl, text;
    std::optional<uint64_t> lastCycle;
    unsigned seed;
public:
    CanonicalTrace(const std::filesystem::path& prefix, unsigned seed);
    void record(const CanonicalEvent& event);
};
} // namespace compactchi
