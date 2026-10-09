#include "Requester.hpp"
#include <algorithm>
#include <stdexcept>

namespace compactchi {
void Requester::require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ReadClass Requester::readClass(unsigned opcode) {
    if (opcode == 3) return ReadClass::Once;
    if (opcode == 4) return ReadClass::Shared;
    if (opcode == 16) return ReadClass::Unique;
    throw std::invalid_argument("requester read profile does not support this opcode");
}
Requester::Requester(ArchitecturalBoard& board, unsigned l2Node, size_t capacity)
    : board(board), l2Node(l2Node), capacity(capacity) {
    require(capacity != 0, "requester needs bounded queue capacity");
}
bool Requester::requestReady() const { return requests.size() + progress.size() < capacity; }
void Requester::enqueue(const RequestFlit& request) {
    require(requestReady(), "requester queue capacity exceeded");
    require(request.target == l2Node && !(request.address & 63) && request.txn < 256,
            "invalid requester route, line framing or Compact TxnID");
    readClass(request.opcode);
    const RequestKey key{request.node, request.txn};
    require(!progress.count(key), "requester reused a live node/TxnID");
    for (const auto& pending : requests)
        require(pending.node != request.node || pending.txn != request.txn, "requester duplicated an offered node/TxnID");
    requests.push_back(request);
}
void Requester::requestAccepted() {
    require(requestValid(), "requester accepted an absent REQ");
    const auto flit = request();
    const RequestKey key{flit.node, flit.txn};
    board.issue(key, readClass(flit.opcode), flit.address, flit.traceTag);
    progress.emplace(key, RequestProgress{flit});
    requests.pop_front();
}
void Requester::receiveData(const DataFlit& response) {
    require(response.node == l2Node && response.opcode == 0 && response.respErr == 0,
            "misrouted, non-CompData or erroneous upstream data in the normal read profile");
    const RequestKey key{response.target, response.txn};
    auto it = progress.find(key);
    require(it != progress.end(), "requester received data for an unknown node/TxnID");
    auto& pending = it->second;
    const auto copyKey = std::make_pair(response.target, pending.flit.address);
    auto old = copies.find(copyKey);
    const bool retainDirty = pending.flit.opcode == 16 && old != copies.end() &&
        old->second.permission == Permission::UD;
    board.data(key, response.dataId, response.dbid, response.resp, response.traceTag, response.data.data());
    std::copy(response.data.begin(), response.data.end(), pending.received.begin() + response.dataId * 32);
    pending.mask |= 1U << response.dataId;
    if (pending.mask == 3) {
        if (pending.flit.opcode != 3) {
            const auto permission = response.resp == 1 ? Permission::SC :
                response.resp == 2 ? Permission::UC : Permission::UD;
            if (!retainDirty) copies[copyKey] = {permission, pending.received};
            // DBID is a routing ID, distinct from both TxnID and TraceTag.
            ackTransactions.emplace(std::make_pair(response.target, response.dbid), pending.flit.txn);
            responses.push_back({{response.target, l2Node, response.dbid, 0, 0, response.traceTag}, false});
            // Keep the requester transaction slot until the CompAck handshake.
        } else progress.erase(it);
    }
}
void Requester::responseAccepted() {
    require(responseValid(), "requester accepted an absent RSP");
    const auto outgoing = responses.front();
    if (outgoing.snoop) {
        const RequestKey key{outgoing.flit.node, outgoing.flit.txn};
        board.snoopResponse(key, outgoing.flit.resp, outgoing.flit.traceTag);
        const auto address = snoopAddresses.at(key);
        auto copy = copies.find({key.node, address});
        if (copy != copies.end()) {
            if ((outgoing.flit.resp & 3) == 0) copies.erase(copy);
            else copy->second.permission = (outgoing.flit.resp & 3) == 1 ? Permission::SC : Permission::UC;
        }
        snoopAddresses.erase(key);
    } else {
        const auto owner = ackTransactions.find({outgoing.flit.node, outgoing.flit.txn});
        require(owner != ackTransactions.end(), "CompAck lost its DBID mapping");
        auto it = progress.find({outgoing.flit.node, owner->second});
        require(it != progress.end() && it->second.mask == 3, "CompAck lost its requester transaction owner");
        board.compAck(outgoing.flit.node, outgoing.flit.txn, outgoing.flit.traceTag);
        progress.erase(it);
        ackTransactions.erase(owner);
    }
    responses.pop_front();
}
bool Requester::snoopReady() const {
    return snoopAddresses.size() < capacity;
}
void Requester::receiveSnoop(const SnoopFlit& snoop) {
    require(snoopReady(), "requester Snoop capacity exceeded");
    require(snoop.node == l2Node && snoop.opcode < 4 && !(snoop.address & 63),
            "invalid Compact Snoop route, opcode or address");
    const RequestKey key{snoop.target, snoop.txn};
    board.issueSnoop(key, snoop.opcode, snoop.address, snoop.traceTag);
    snoopAddresses.emplace(key, snoop.address);
    const auto it = copies.find({snoop.target, snoop.address});
    const bool present = it != copies.end();
    const bool dirty = present && it->second.permission == Permission::UD;
    unsigned resp = 0;
    if (present && snoop.opcode >= 2)
        resp = snoop.opcode == 2 || it->second.permission == Permission::SC ? 1 : 2;
    if (dirty && snoop.opcode != 0) {
        resp |= 4;
        for (unsigned beat = 0; beat < 2; ++beat) {
            DataFlit response{snoop.target, l2Node, snoop.txn, 0, beat, resp, snoop.traceTag};
            response.opcode = 3; // Compact SnpRespData
            std::copy_n(it->second.data.begin() + beat * 32, 32, response.data.begin());
            dataResponses.push_back({response, snoop.opcode, snoop.address});
        }
    } else responses.push_back({{snoop.target, l2Node, snoop.txn, 1, resp, snoop.traceTag}, true});
}
void Requester::dataAccepted() {
    require(dataValid(), "requester accepted absent Snoop data");
    const auto outgoing = dataResponses.front();
    const auto& flit = outgoing.flit;
    const RequestKey key{flit.node, flit.txn};
    board.snoopData(key, flit.dataId, flit.resp, flit.traceTag, flit.data.data());
    if (flit.dataId == 1) {
        auto copy = copies.find({flit.node, outgoing.address});
        require(copy != copies.end(), "dirty Snoop data lost the node copy");
        if ((flit.resp & 3) == 0) copies.erase(copy);
        else copy->second.permission = (flit.resp & 3) == 1 ? Permission::SC : Permission::UC;
        snoopAddresses.erase(key);
    }
    dataResponses.pop_front();
}
void Requester::store(unsigned node, uint64_t address, const LineData& data) {
    board.store(node, address, data);
    auto it = copies.find({node, address});
    require(it != copies.end(), "requester store has no node data copy");
    it->second.data = data;
    it->second.permission = Permission::UD;
}
bool Requester::idle() const {
    return requests.empty() && progress.empty() && responses.empty() && dataResponses.empty() &&
           snoopAddresses.empty() && ackTransactions.empty();
}
} // namespace compactchi
