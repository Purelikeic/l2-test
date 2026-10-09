#include "ArchitecturalBoard.hpp"
#include <algorithm>
#include <cstring>

namespace compactchi {
void ArchitecturalBoard::require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ArchitecturalBoard::ArchitecturalBoard(TLLocalContext& ctx, unsigned count)
    : context(ctx), nodeCount(count), transient(count) {
    require(count != 0, "scoreboard requires at least one node");
}
NodeLine& ArchitecturalBoard::nodeLine(uint64_t address) {
    auto& entries = nodes.get();
    if (!entries.count(address)) entries.emplace(address, std::make_shared<NodeLine>());
    return *entries.at(address);
}
void ArchitecturalBoard::initialize(uint64_t address, const LineData& data) {
    require(!(address & 63) && !global.haskey(address), "invalid or repeated initial line");
    auto entry = std::make_shared<Global_SBEntry>();
    entry->status = Global_SBEntry::SB_VALID;
    entry->data = make_shared_tldata<64>();
    std::copy(data.begin(), data.end(), entry->data->data);
    global.update(&context, address, entry);
}
void ArchitecturalBoard::issue(RequestKey key, ReadClass request, uint64_t address, bool traceTag) {
    require(key.node < nodeCount, "unknown requester identity");
    require(!(address & 63) && global.haskey(address), "request needs a known aligned line");
    require(!pending.haskey(key), "node reused a live TxnID");
    require(!lineOwners.count(address), "same-line request must wait for prior terminal obligation");
    const auto& state = nodeLine(address);
    if (request != ReadClass::Once) {
        require(!state.owner || *state.owner == key.node, "illegal second persistent coherent owner");
        if (request == ReadClass::Shared)
            require(state.permission == Permission::I || state.permission == Permission::SC,
                    "ReadShared requires requester I or SC");
    } else {
        require(!state.owner || *state.owner != key.node, "ReadOnce requires requester Invalid");
    }
    auto requestEntry = std::make_shared<ReadExpectation>();
    requestEntry->request = request;
    requestEntry->address = address;
    requestEntry->startCycle = context.cycle();
    requestEntry->traceTag = traceTag;
    std::copy_n(global.query(&context, address)->data->data, 64, requestEntry->expected.begin());
    pending.update(&context, key, requestEntry);
    lineOwners.emplace(address, key);
    if (request == ReadClass::Once) {
        auto snapshot = make_shared_tldata<64>();
        std::copy(requestEntry->expected.begin(), requestEntry->expected.end(), snapshot->data);
        transient.allocate(&context, key.node, address, key.txn, snapshot);
    }
}
void ArchitecturalBoard::data(RequestKey key, unsigned dataId, unsigned dbid, unsigned resp,
                              bool traceTag, const uint8_t* bytes) {
    require(pending.haskey(key), "data response targets unknown node/TxnID");
    auto entry = pending.query(&context, key);
    require(dataId < 2 && !(entry->beats & (1U << dataId)), "duplicate or invalid data beat");
    require(traceTag == entry->traceTag, "response TraceTag mismatch");
    require(!entry->dbid || *entry->dbid == dbid, "response DBID changed across beats");
    require(!entry->resp || *entry->resp == resp, "response permission changed across beats");
    if (entry->request == ReadClass::Once)
        require(resp == 0 || resp == 2, "ReadOnce returned a persistent or dirty permission");
    else if (entry->request == ReadClass::Shared)
        require(resp == 1 || resp == 2 || resp == 6, "ReadShared returned an illegal permission");
    else require(resp == 2 || resp == 6, "ReadUnique returned an illegal permission");
    require(bytes != nullptr && std::equal(bytes, bytes + 32, entry->expected.begin() + dataId * 32),
            "architectural read data mismatch");
    if (entry->request != ReadClass::Once && !entry->dbid)
        require(!acks.count({key.node, dbid}), "response reused a live node/DBID");
    entry->dbid = dbid;
    entry->resp = resp;
    entry->beats |= 1U << dataId;
    if (entry->request != ReadClass::Once) acks[{key.node, dbid}] = key;
    if (entry->beats == 3) {
        if (entry->request == ReadClass::Once) {
            auto fullData = make_shared_tldata<64>();
            std::copy(entry->expected.begin(), entry->expected.end(), fullData->data);
            transient.verify(&context, key.node, entry->address, key.txn, fullData);
            transient.finish(&context, key.node, entry->address, key.txn);
            lineOwners.erase(entry->address);
            pending.erase(&context, key);
        } else {
            auto& state = nodeLine(entry->address);
            state.owner = key.node;
            state.permission = resp == 1 ? Permission::SC : resp == 2 ? Permission::UC : Permission::UD;
        }
    }
}
void ArchitecturalBoard::compAck(unsigned node, unsigned dbid, bool traceTag) {
    auto it = acks.find({node, dbid});
    require(it != acks.end(), "unknown node/DBID CompAck");
    const auto key = it->second;
    auto entry = pending.query(&context, key);
    // Conservative AnserL2 integration waits for both beats, as documented.
    require(entry->beats == 3, "CompAck preceded complete data");
    require(entry->traceTag == traceTag, "CompAck TraceTag mismatch");
    lineOwners.erase(entry->address);
    acks.erase(it);
    pending.erase(&context, key);
}
void ArchitecturalBoard::store(unsigned node, uint64_t address, const LineData& data) {
    auto& state = nodeLine(address);
    require(state.owner && *state.owner == node &&
            (state.permission == Permission::UC || state.permission == Permission::UD),
            "store without Unique permission");
    require(!lineOwners.count(address), "store before transaction terminal completion");
    auto entry = std::make_shared<Global_SBEntry>();
    entry->status = Global_SBEntry::SB_VALID;
    entry->data = make_shared_tldata<64>();
    std::copy(data.begin(), data.end(), entry->data->data);
    global.update(&context, address, entry);
    state.permission = Permission::UD;
}
bool ArchitecturalBoard::idle() {
    return pending.get().empty() && acks.empty() && lineOwners.empty();
}
} // namespace compactchi
