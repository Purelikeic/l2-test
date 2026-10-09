#pragma once

#include "../Utils/ScoreBoard.h"
#include "../Utils/ULScoreBoard.h"
#include "../Sequencer/TestNode.hpp"
#include <array>
#include <map>
#include <optional>
#include <stdexcept>

namespace compactchi {
using LineData = std::array<uint8_t, DATASIZE>;
enum class Permission { I, SC, UC, UD };
enum class ReadClass { Once, Shared, Unique };

struct RequestKey {
    unsigned node, txn;
    bool operator<(const RequestKey& b) const { return std::tie(node, txn) < std::tie(b.node, b.txn); }
    operator uint64_t() const { return (uint64_t(node) << 32) | txn; }
};
struct ReadExpectation {
    ReadClass request;
    uint64_t address, startCycle;
    bool traceTag;
    bool retainDirtyData = false;
    unsigned beats = 0;
    std::optional<unsigned> dbid;
    std::optional<unsigned> resp;
    LineData expected;
};
struct NodeLine {
    Permission permission = Permission::I;
    std::optional<unsigned> owner;
};
struct SnoopExpectation {
    unsigned opcode;
    uint64_t address;
    bool traceTag, dirty;
    Permission before = Permission::I;
    unsigned beats = 0;
    std::optional<unsigned> resp;
    LineData expected;
};

/** Initial independent read/node obligations on the original scoreboard storage.
 * Rules: CPL2 tl2chi/MainPipe.scala need_probe_s3_a and client metadata;
 * Compact CHI guide 8.2/10.2 legal permissions, ReadOnce persistence and CompAck.
 * Dirty data is authoritative in GlobalBoard, independent of Home memory.
 * This node component has no replacement, retry, CMO, DCT or internal L2-state
 * model yet and cannot establish full architectural alignment on its own. */
class ArchitecturalBoard {
    TLLocalContext& context;
    unsigned nodeCount;
    GlobalBoard<paddr_t> ownedGlobal;
    UncachedBoard<paddr_t> ownedTransient;
    GlobalBoard<paddr_t>& global;
    UncachedBoard<paddr_t>& transient;
    ScoreBoard<uint64_t, ReadExpectation> pending;
    ScoreBoard<uint64_t, NodeLine> nodes;
    std::map<std::pair<unsigned, unsigned>, RequestKey> acks;
    std::map<uint64_t, RequestKey> lineOwners;
    ScoreBoard<uint64_t, SnoopExpectation> snoops;
    std::map<uint64_t, RequestKey> snoopOwners;
    struct Control {
        TestAction action;
        unsigned sent = 0, received = 0;
        bool completion = false;
        std::optional<unsigned> dbid;
        LineData expected{};
    };
    std::map<RequestKey, Control> controls;
    std::map<uint64_t, LineData> deviceExpected, memoryExpected;
    void checkSnoop(const SnoopExpectation& entry, unsigned resp, bool traceTag, bool withData);
    void finishSnoop(RequestKey key, const SnoopExpectation& entry, unsigned resp);
    NodeLine& nodeLine(uint64_t address);
    static void require(bool value, const char* message);
public:
    explicit ArchitecturalBoard(TLLocalContext& context, unsigned nodeCount);
    ArchitecturalBoard(TLLocalContext& context, unsigned nodeCount,
                       GlobalBoard<paddr_t>& global, UncachedBoard<paddr_t>& uncached);
    void initialize(uint64_t address, const LineData& data);
    void issue(RequestKey key, ReadClass request, uint64_t address, bool traceTag);
    void data(RequestKey key, unsigned dataId, unsigned dbid, unsigned resp,
              bool traceTag, const uint8_t* bytes);
    void compAck(unsigned node, unsigned dbid, bool traceTag);
    void store(unsigned node, uint64_t address, const LineData& data);
    void issueSnoop(RequestKey key, unsigned opcode, uint64_t address, bool traceTag);
    void snoopResponse(RequestKey key, unsigned resp, bool traceTag);
    void snoopData(RequestKey key, unsigned dataId, unsigned resp, bool traceTag, const uint8_t* bytes);
    const NodeLine& state(uint64_t address) { return nodeLine(address); }
    bool canIssue(unsigned node, uint64_t address, bool persistent);
    bool lineBusy(uint64_t address);
    void issueControl(RequestKey key, const TestAction& action);
    void controlDBID(RequestKey key, unsigned dbid, bool traceTag);
    void controlSent(RequestKey key, unsigned beat);
    bool controlData(RequestKey key, unsigned beat, unsigned resp, bool traceTag, const uint8_t* bytes);
    void controlComplete(RequestKey key, unsigned opcode, unsigned resp, bool traceTag);
    void controlAck(RequestKey key);
    std::pair<LineData, uint64_t> deviceWriteExpectation(uint64_t address);
    void memoryCommitted(uint64_t address, const LineData& expected) { memoryExpected[address] = expected; }
    const LineData& expectedDevice(uint64_t address);
    bool idle();
};
} // namespace compactchi
