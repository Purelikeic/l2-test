#pragma once

#include "EbHome.hpp"
#include "ArchitecturalBoard.hpp"
#include <map>

namespace compactchi {
struct LowerSnoop {
    unsigned txn, opcode;
    uint64_t address;
    bool retToSrc = false, traceTag = false;
    unsigned forwardNode = 0, forwardTxn = 0;
};
enum class DirtySink { None, Dut, Home, ForwardNode, Discarded };
struct SnoopResult {
    Permission dutAfter;
    DirtySink dirtySink;
    Permission forwardAfter = Permission::I;
    LineData latest;
};

/** Independent stable-line lower Snoop obligations. Expected state must come
 * from the architectural model/scenario, never from DUT metadata. Rules derive
 * from CPL2 tl2chi/MainPipe.scala respCacheState/respPassDirty/fwdState and
 * tl2chi/MSHR.scala mp_dct routing. This profile intentionally excludes nested
 * release/CMO, errors and the remaining Snoop opcodes; it is not an RTL model.
 * Memory writes and forward-node residency are actions for the composite Home
 * model, not silently performed or claimed by this protocol checker. */
class LowerSnoopBoard {
    struct Entry {
        LowerSnoop request;
        SnoopResult result;
        bool homeData, dct, homeDone = false, forwardAck = false;
        unsigned homeResp, forwardResp, homeMask = 0, forwardMask = 0;
    };
    TLLocalContext& context;
    GlobalBoard<paddr_t>& global;
    unsigned dut, home;
    size_t capacity;
    std::map<unsigned, Entry> active;
    std::map<std::pair<unsigned, unsigned>, unsigned> forward;
    static void require(bool condition, const char* message);
    static bool forwarded(unsigned opcode);
    void checkBytes(const Entry& entry, const EbData& data, unsigned mask);
public:
    LowerSnoopBoard(TLLocalContext& context, GlobalBoard<paddr_t>& global,
                    unsigned dut = 1, unsigned home = 8, size_t capacity = 16);
    void issue(const LowerSnoop& request, Permission expectedBefore);
    void response(const EbResponse& rsp);
    void homeData(const EbData& data);
    void forwardData(const EbData& data);
    void forwardCompAck(unsigned node, unsigned target, unsigned dbid, bool traceTag);
    bool complete(unsigned txn) const;
    SnoopResult retire(unsigned txn);
    bool idle() const { return active.empty() && forward.empty(); }
};
} // namespace compactchi
