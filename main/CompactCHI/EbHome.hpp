#pragma once

#include "../Memory/CHIHomeMemory.hpp"
#include "../Utils/ScoreBoard.h"
#include <deque>
#include <map>
#include <tuple>

namespace compactchi {
struct EbRequest {
    unsigned source, target, txn, opcode;
    uint64_t address;
    bool traceTag = false;
    bool expCompAck = true;
    unsigned size = 6;
    bool allowRetry = true;
    unsigned pCrdType = 0;
    unsigned memAttr = 0xd, order = 0;
    bool ns = false, likelyShared = false;
};
struct EbResponse {
    unsigned source, target, txn, opcode, dbid, resp;
    bool traceTag;
    unsigned pCrdType = 0;
};
struct EbData {
    unsigned source, target, txn, opcode, dbid, home, dataId, resp;
    bool traceTag;
    std::array<uint8_t, 32> data{};
    uint32_t byteEnable = 0xffffffffU;
    unsigned respErr = 0;
};
struct HomePlan {
    bool separated = false;
    bool completionAfterData = false;
    bool upperBeatFirst = false;
    bool cleanCopyback = false;
    unsigned completionSource = 0; // zero selects this Home node
    bool responseTraceTag = false;
    bool retryOnce = false;
    bool creditBeforeRetry = false;
    unsigned creditType = 3;
    unsigned retryIssuer = 0; // zero selects this Home node
};

/** Normal cacheable E.b Home transport on the original MemoryAgent backend.
 * Read/clean-release completion identities derive from CPL2 tl2chi/MSHR.scala
 * CompData, RespSepData and WriteEvictOrEvict handling. GlobalBoard supplies
 * independent copyback expectations; outgoing read data comes from real memory.
 * Cacheable Retry/P-Credit is modeled by issuer/type rather than TxnID.
 * Lower Snoop/DCT, CMO and MMIO are not implemented by this profile yet. */
class EbHome {
    struct Context {
        EbRequest request;
        HomePlan plan;
        unsigned dbid, ackTarget;
        bool write, copyback, needAck, rspAccepted = false, memoryPending = false;
        unsigned sentMask = 0, receivedMask = 0;
        bool retryPending = false, retryDelivered = false;
        chi_agent::Line expected{}, received{};
    };
    enum class RspKind { Completion, Retry, Credit };
    struct QueuedRsp { EbResponse flit; unsigned owner; RspKind kind = RspKind::Completion; };
    struct QueuedDat { EbData flit; unsigned owner; };
    chi_agent::HomeMemory& memory;
    TLLocalContext& local;
    GlobalBoard<paddr_t>& global;
    unsigned node;
    size_t capacity;
    unsigned nextOwner = 1, nextDbid = 128;
    std::map<unsigned, Context> contexts;
    // Read acquisition and victim release can share a DUT MSHR TxnID while
    // their different DBID obligations are live. Do not collapse these roles.
    std::map<std::tuple<unsigned, unsigned, bool>, unsigned> transactions;
    std::map<unsigned, unsigned> dbids;
    std::map<std::pair<unsigned, unsigned>, unsigned> credits;
    std::deque<QueuedRsp> responses;
    std::deque<QueuedDat> data;
    static bool isRead(unsigned opcode);
    static bool isWrite(unsigned opcode);
    static void require(bool value, const char* message);
    void finish(unsigned owner);
    void enqueueCompletion(unsigned owner);
    void start(unsigned owner);
public:
    EbHome(chi_agent::HomeMemory& memory, TLLocalContext& local, GlobalBoard<paddr_t>& global,
           unsigned node = 8, size_t capacity = 16);
    bool requestReady(const EbRequest& request) const;
    void accept(const EbRequest& request, const HomePlan& plan = {});
    // Call after capturing memory handshakes, before memory next-cycle outputs.
    void service();
    bool responseValid() const { return !responses.empty(); }
    const EbResponse& response() const { return responses.front().flit; }
    void responseAccepted();
    bool dataValid() const { return !data.empty(); }
    const EbData& datum() const { return data.front().flit; }
    void dataAccepted();
    void receiveCompAck(unsigned source, unsigned target, unsigned dbid, bool traceTag);
    void receiveCopyback(const EbData& flit);
    bool idle() const;
};
} // namespace compactchi
