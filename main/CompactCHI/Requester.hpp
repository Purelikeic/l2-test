#pragma once

#include "ArchitecturalBoard.hpp"
#include <deque>
#include <map>

namespace compactchi {
struct RequestFlit {
    unsigned node, target, txn, opcode;
    uint64_t address;
    bool traceTag;
    unsigned size = 6;
};
struct DataFlit {
    unsigned node, target, txn, dbid, dataId, resp;
    bool traceTag;
    std::array<uint8_t, 32> data{};
    unsigned opcode = 0;
    unsigned byteEnable = 0xffffffffU;
    unsigned respErr = 0;
};
struct ResponseFlit {
    unsigned node, target, txn, opcode, resp;
    bool traceTag;
};
struct SnoopFlit {
    unsigned node, target, txn, opcode;
    uint64_t address;
    bool traceTag;
};

/** Read-profile requester and persistent node adapter. Transport identity and
 * queues are separate from the independent architectural board. Snoop responses
 * and CompAck never wait for the request queue to make progress. */
class Requester {
    struct RequestProgress { RequestFlit flit; unsigned mask = 0; LineData received{}; };
    struct OutResponse { ResponseFlit flit; bool snoop; };
    struct OutData { DataFlit flit; unsigned snoopOpcode; uint64_t address; bool control = false; RequestKey owner{}; };
    struct NodeCopy { Permission permission; LineData data; };
    ArchitecturalBoard& board;
    unsigned l2Node;
    std::vector<TestNodeKind> nodeKinds;
    size_t capacity;
    std::deque<RequestFlit> requests;
    std::deque<OutResponse> responses;
    std::deque<OutData> dataResponses;
    std::map<RequestKey, RequestProgress> progress;
    std::map<std::pair<unsigned, uint64_t>, NodeCopy> copies;
    std::map<RequestKey, uint64_t> snoopAddresses;
    struct ControlProgress { RequestFlit flit; TestAction action; bool accepted = false, dbidReceived = false, combined = false; };
    std::map<RequestKey, ControlProgress> controls;
    std::deque<RequestFlit> events;
    std::map<std::pair<unsigned, unsigned>, unsigned> ackTransactions;
    static ReadClass readClass(unsigned opcode);
    static void require(bool value, const char* message);
public:
    Requester(ArchitecturalBoard& board, unsigned l2Node, size_t capacity = 16);
    void configureNodes(const std::vector<TestNodeDescriptor>& nodes) {
        nodeKinds.clear();
        for (const auto& node : nodes) {
            if (node.id != nodeKinds.size()) throw std::invalid_argument("noncanonical node layout");
            nodeKinds.push_back(node.kind);
        }
    }
    bool requestReady() const;
    bool ready(unsigned node, const TestAction& action);
    void enqueueAction(unsigned node, const TestAction& action);
    bool eventValid() const { return !events.empty(); }
    const RequestFlit& event() const { return events.front(); }
    void eventAccepted();
    void receiveResponse(const ResponseFlit& response, unsigned dbid, unsigned respErr);
    bool nodeIdle(unsigned node) const;
    void enqueue(const RequestFlit& request);
    bool requestValid() const { return !requests.empty(); }
    const RequestFlit& request() const { return requests.front(); }
    void requestAccepted();
    void receiveData(const DataFlit& response);
    bool responseValid() const { return !responses.empty(); }
    const ResponseFlit& response() const { return responses.front().flit; }
    void responseAccepted();
    bool dataValid() const { return !dataResponses.empty(); }
    const DataFlit& data() const { return dataResponses.front().flit; }
    void dataAccepted();
    bool snoopReady() const;
    void receiveSnoop(const SnoopFlit& snoop);
    void store(unsigned node, uint64_t address, const LineData& data);
    bool idle() const;
};
} // namespace compactchi
