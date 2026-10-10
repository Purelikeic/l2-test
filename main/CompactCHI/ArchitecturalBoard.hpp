#pragma once

#include "../Utils/ScoreBoard.h"
#include "../Utils/ULScoreBoard.h"
#include "../Sequencer/TestNode.hpp"
#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <ostream>

namespace compactchi {
using LineData = std::array<uint8_t, DATASIZE>;
enum class Permission { I, SC, UC, UD };
enum class ReadClass { Once, Shared, Unique };

struct ResidentLine {
    Permission permission;
    LineData data;
    bool operator==(const ResidentLine&) const = default;
};
// Stable architectural storage, separate from all transport/resource ownership.
struct ArchitecturalState {
    std::map<std::pair<unsigned, uint64_t>, ResidentLine> requesters;
    std::map<uint64_t, LineData> memory, devices;
};

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

/** Independent data/permission and terminal node/Home obligations.
 * Rules: CPL2 tl2chi/MainPipe.scala client metadata and Compact CHI guide
 * 7, 8 and 10. Latest cache data and committed Home data are separate versions.
 * Terminal observations never feed expected data or functional admission.
 * Internal L2 residency and complete protocol/topology modeling remain open. */
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
    ArchitecturalState expectedTerminal();
    void checkTerminal(const ArchitecturalState& observed, std::ostream* evidence = nullptr);
    bool idle();
};
} // namespace compactchi
