#include "Requester.hpp"
#include <algorithm>
#include <set>

namespace compactchi {
bool Requester::nodeIdle(unsigned node) const {
    for (const auto& r : requests) if (r.node == node) return false;
    for (const auto& r : events) if (r.node == node) return false;
    for (const auto& [key, value] : progress) if (key.node == node) return false;
    for (const auto& [key, value] : controls) if (key.node == node) return false;
    for (const auto& [key, value] : snoopAddresses) if (key.node == node) return false;
    return true;
}
unsigned Requester::outstanding(unsigned node) const {
    std::set<RequestKey> keys;
    for (const auto& r : requests) if (r.node == node) keys.insert({r.node, r.txn});
    for (const auto& r : events) if (r.node == node) keys.insert({r.node, r.txn});
    for (const auto& [k, value] : progress) if (k.node == node) keys.insert(k);
    for (const auto& [k, value] : controls) if (k.node == node) keys.insert(k);
    return keys.size();
}
bool Requester::lineIdle(uint64_t address) const {
    const auto line = address & ~uint64_t(63);
    for (const auto& r : requests) if ((r.address & ~uint64_t(63)) == line) return false;
    for (const auto& r : events) if ((r.address & ~uint64_t(63)) == line) return false;
    for (const auto& [k, r] : progress) if ((r.flit.address & ~uint64_t(63)) == line) return false;
    for (const auto& [k, r] : controls) if ((r.action.address & ~uint64_t(63)) == line) return false;
    for (const auto& [k, a] : snoopAddresses) if ((a & ~uint64_t(63)) == line) return false;
    return true;
}
bool Requester::ready(unsigned node, const TestAction& action) {
    const bool persistent = action.kind == TestActionKind::AcquireShared || action.kind == TestActionKind::AcquireUnique ||
        action.kind == TestActionKind::MakeUnique;
    if (!requestReady() || (perNodeLimit == 1 ? !nodeIdle(node) : outstanding(node) >= perNodeLimit)) return false;
    if (!lineIdle(action.address)) return false;
    const RequestKey key{node, action.txn};
    if (progress.count(key) || controls.count(key)) return false;
    for (const auto& r : requests) if (r.node == node && r.txn == action.txn) return false;
    for (const auto& r : events) if (r.node == node && r.txn == action.txn) return false;
    const bool device = action.kind == TestActionKind::ReadNoSnp || action.kind == TestActionKind::WriteNoSnp;
    if (!device && !board.canIssue(node, action.address, persistent)) return false;
    for (const auto& request : requests) if ((request.address & ~uint64_t(63)) == (action.address & ~uint64_t(63))) return false;
    for (const auto& [key, c] : controls) if ((c.action.address & ~uint64_t(63)) == (action.address & ~uint64_t(63))) return false;
    return true;
}
void Requester::enqueueAction(unsigned node, const TestAction& action) {
    require(ready(node, action), "action attempted before node/line admission");
    if (action.kind == TestActionKind::Store) { store(node, action.address, action.data); return; }
    unsigned opcode;
    switch (action.kind) {
    case TestActionKind::AcquireShared: opcode = 4; break;
    case TestActionKind::AcquireUnique: opcode = 16; break;
    case TestActionKind::ReadOnce: opcode = 3; break;
    case TestActionKind::MakeUnique: opcode = 18; break;
    case TestActionKind::Clean: opcode = 12; break;
    case TestActionKind::Flush: opcode = 13; break;
    case TestActionKind::Invalidate: opcode = 14; break;
    case TestActionKind::Evict: opcode = 0; break;
    case TestActionKind::WriteBack: opcode = 1; break;
    case TestActionKind::ReadNoSnp: opcode = 2; break;
    case TestActionKind::WriteNoSnp: opcode = action.size == 6 ? 9 : 8; break;
    default: throw std::invalid_argument("Exclusive/Atomic/WriteUnique are never approximated");
    }
    RequestFlit flit{node, l2Node, action.txn, opcode, action.address, action.traceTag, action.size};
    if (action.kind == TestActionKind::AcquireShared || action.kind == TestActionKind::AcquireUnique || action.kind == TestActionKind::ReadOnce) {
        enqueue(flit); return;
    }
    auto copy = copies.find({node, action.address});
    TestAction intent = action;
    if (action.kind == TestActionKind::Evict || action.kind == TestActionKind::WriteBack) {
        require(copy != copies.end(), "release lacks a node copy");
        require(action.kind == TestActionKind::WriteBack ? copy->second.permission == Permission::UD :
            copy->second.permission != Permission::UD, "release classification differs from node permission");
        intent.data = copy->second.data;
    }
    const RequestKey key{node, action.txn};
    require(!controls.count(key) && !progress.count(key), "action reused a live node/TxnID");
    controls.emplace(key, ControlProgress{flit, intent});
    if (action.kind == TestActionKind::Evict || action.kind == TestActionKind::WriteBack) events.push_back(flit);
    else requests.push_back(flit);
}
void Requester::eventAccepted() {
    require(eventValid(), "absent EVT accepted");
    const auto event = events.front();
    const RequestKey key{event.node, event.txn};
    auto& c = controls.at(key);
    board.issueControl(key, c.action);
    c.accepted = true;
    if (c.action.kind == TestActionKind::Evict) copies.erase({event.node, event.address});
    events.pop_front();
}
void Requester::receiveResponse(const ResponseFlit& response, unsigned dbid, unsigned respErr) {
    require(response.node == l2Node && respErr == 0, "misrouted/erroneous node completion");
    const RequestKey key{response.target, response.txn};
    auto& c = controls.at(key);
    require(c.accepted, "control response preceded request handshake");
    if (response.opcode == 2 || response.opcode == 3) {
        board.controlDBID(key, dbid, response.traceTag);
        if (response.opcode == 3) {
            require(c.action.kind == TestActionKind::WriteBack, "unexpected combined upstream completion");
            board.controlComplete(key, response.opcode, response.resp, response.traceTag);
            c.combined = true;
        }
        c.dbidReceived = true;
        const bool release = c.action.kind == TestActionKind::WriteBack;
        const unsigned first = c.action.size == 6 ? 0 : (c.action.address >> 5) & 1;
        const unsigned count = c.action.size == 6 ? 2 : 1;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned beat = first + i;
            DataFlit data{key.node, l2Node, dbid, 0, beat, release ? 6U : 0U, response.traceTag};
            data.opcode = release ? 2 : 0;
            data.byteEnable = uint32_t(c.action.byteEnable >> (beat * 32));
            std::copy_n(c.action.data.begin() + beat * 32, 32, data.data.begin());
            dataResponses.push_back({data, 0, c.action.address, true, key});
        }
    } else {
        board.controlComplete(key, response.opcode, response.resp, response.traceTag);
        if (c.action.kind == TestActionKind::MakeUnique) {
            require(ackTransactions.emplace(std::make_pair(key.node, dbid), key.txn).second, "control DBID reused");
            responses.push_back({{key.node, l2Node, dbid, 0, 0, response.traceTag}, false});
        } else {
            if (c.action.kind == TestActionKind::WriteBack) copies.erase({key.node, c.action.address});
            controls.erase(key);
        }
    }
}
} // namespace compactchi
