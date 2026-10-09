#include "ArchitecturalBoard.hpp"
#include <algorithm>
#include <cstring>

namespace compactchi {
void ArchitecturalBoard::require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ArchitecturalBoard::ArchitecturalBoard(TLLocalContext& ctx, unsigned count)
    : context(ctx), nodeCount(count), ownedTransient(count), global(ownedGlobal), transient(ownedTransient) {
    require(count != 0, "scoreboard requires at least one node");
}
ArchitecturalBoard::ArchitecturalBoard(TLLocalContext& ctx, unsigned count,
                                     GlobalBoard<paddr_t>& board, UncachedBoard<paddr_t>& uncached)
    : context(ctx), nodeCount(count), ownedTransient(0), global(board), transient(uncached) {
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
    memoryExpected[address] = data;
}
void ArchitecturalBoard::issue(RequestKey key, ReadClass request, uint64_t address, bool traceTag) {
    require(key.node < nodeCount, "unknown requester identity");
    require(!(address & 63) && global.haskey(address), "request needs a known aligned line");
    require(!pending.haskey(key), "node reused a live TxnID");
    require(request == ReadClass::Once || !lineOwners.count(address), "same-line persistent request must wait for prior terminal obligation");
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
    requestEntry->retainDirtyData = request == ReadClass::Unique && state.owner == key.node &&
        state.permission == Permission::UD;
    std::copy_n(global.query(&context, address)->data->data, 64, requestEntry->expected.begin());
    pending.update(&context, key, requestEntry);
    if (request != ReadClass::Once) lineOwners.emplace(address, key);
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
    require(bytes != nullptr, "architectural read data is absent");
    // Compact guide 10.2: a Requester already holding UD must discard or merge
    // stale ReadUnique data. Its independently held latest copy remains the
    // architectural value; transport data does not overwrite that copy.
    if (!entry->retainDirtyData)
        require(std::equal(bytes, bytes + 32, entry->expected.begin() + dataId * 32),
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
            pending.erase(&context, key);
        } else {
            auto& state = nodeLine(entry->address);
            state.owner = key.node;
            state.permission = entry->retainDirtyData ? Permission::UD :
                resp == 1 ? Permission::SC : resp == 2 ? Permission::UC : Permission::UD;
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
    require(!lineBusy(address), "store before transaction or Snoop terminal completion");
    auto entry = std::make_shared<Global_SBEntry>();
    entry->status = Global_SBEntry::SB_VALID;
    entry->data = make_shared_tldata<64>();
    std::copy(data.begin(), data.end(), entry->data->data);
    global.update(&context, address, entry);
    state.permission = Permission::UD;
}
void ArchitecturalBoard::issueSnoop(RequestKey key, unsigned opcode, uint64_t address, bool traceTag) {
    require(key.node < nodeCount && opcode < 4 && !(address & 63) && global.haskey(address),
            "invalid Compact Snoop identity, opcode or line");
    require(!snoops.haskey(key) && !snoopOwners.count(address), "Snoop resource already owned");
    const auto requestOwner = lineOwners.find(address);
    if (requestOwner != lineOwners.end() && requestOwner->second.node == key.node && pending.haskey(requestOwner->second)) {
        const auto request = pending.query(&context, requestOwner->second);
        require(request->request == ReadClass::Once || request->beats == 0,
                "Snoop targeted a persistent grant awaiting CompAck");
    }
    const auto& state = nodeLine(address);
    // This adapter models included persistent nodes; a targeted node must be
    // either the sole owner or Invalid. Clean invalid-node replies need no data.
    const bool owner = state.owner && *state.owner == key.node;
    auto entry = std::make_shared<SnoopExpectation>();
    entry->opcode = opcode;
    entry->address = address;
    entry->traceTag = traceTag;
    entry->dirty = owner && state.permission == Permission::UD;
    entry->before = owner ? state.permission : Permission::I;
    std::copy_n(global.query(&context, address)->data->data, 64, entry->expected.begin());
    snoops.update(&context, key, entry);
    snoopOwners.emplace(address, key);
}
void ArchitecturalBoard::checkSnoop(const SnoopExpectation& entry, unsigned resp,
                                  bool traceTag, bool withData) {
    require(entry.traceTag == traceTag, "Snoop response TraceTag mismatch");
    const unsigned state = resp & 3;
    const bool passDirty = (resp & 4) != 0;
    require(resp <= 6 && state != 3, "reserved Compact Snoop response state");
    if (entry.before == Permission::I)
        require(!withData && resp == 0, "Invalid snoopee returned data or valid permission");
    if (entry.before == Permission::SC && entry.opcode == 3)
        require(state != 2, "clean Snoop promoted Shared to Unique");
    if (entry.opcode < 2) require(state == 0, "invalidating Snoop kept a valid node state");
    if (entry.opcode == 2) require(state == 0 || state == 1, "shared Snoop kept Unique permission");
    if (entry.opcode == 0) require(!withData && !passDirty, "discard Snoop returned data");
    if (entry.dirty && entry.opcode != 0)
        require(withData && passDirty, "dirty Snoop failed to return dirty responsibility and data");
    else require(!passDirty, "clean Snoop fabricated dirty responsibility");
}
void ArchitecturalBoard::finishSnoop(RequestKey key, const SnoopExpectation& entry, unsigned resp) {
    auto& state = nodeLine(entry.address);
    // A probe of an Invalid identity must not erase another identity's owner.
    const auto requestOwner = lineOwners.find(entry.address);
    if (requestOwner != lineOwners.end() && requestOwner->second.node == key.node && pending.haskey(requestOwner->second)) {
        auto request = pending.query(&context, requestOwner->second);
        // Losing or cleaning UD before the first grant means the requester
        // can no longer discard the completion as an already-dirty copy.
        request->retainDirtyData = false;
    }
    if (state.owner && *state.owner == key.node) {
        const unsigned base = resp & 3;
        if (base == 0) state = {};
        else state.permission = base == 1 ? Permission::SC : Permission::UC;
    } else require((resp & 3) == 0, "Invalid snoopee acquired a line through Snoop response");
    snoopOwners.erase(entry.address);
    snoops.erase(&context, key);
}
void ArchitecturalBoard::snoopResponse(RequestKey key, unsigned resp, bool traceTag) {
    require(snoops.haskey(key), "unknown node/Snoop TxnID");
    auto entry = snoops.query(&context, key);
    require(entry->beats == 0, "Snoop mixed a response with partial data");
    checkSnoop(*entry, resp, traceTag, false);
    finishSnoop(key, *entry, resp);
}
void ArchitecturalBoard::snoopData(RequestKey key, unsigned dataId, unsigned resp,
                                 bool traceTag, const uint8_t* bytes) {
    require(snoops.haskey(key), "unknown node/Snoop data TxnID");
    auto entry = snoops.query(&context, key);
    require(dataId < 2 && !(entry->beats & (1U << dataId)), "duplicate or invalid Snoop data beat");
    checkSnoop(*entry, resp, traceTag, true);
    require(!entry->resp || *entry->resp == resp, "Snoop response changed across data beats");
    require(bytes && std::equal(bytes, bytes + 32, entry->expected.begin() + dataId * 32),
            "Snoop did not return the independently expected latest data");
    entry->resp = resp;
    entry->beats |= 1U << dataId;
    if (entry->beats == 3) finishSnoop(key, *entry, resp);
}
bool ArchitecturalBoard::lineBusy(uint64_t address) {
    for (const auto& entry : pending.get()) if (entry.second->address == address) return true;
    for (const auto& entry : controls) if ((entry.second.action.address & ~uint64_t(63)) == address) return true;
    return snoopOwners.count(address);
}
bool ArchitecturalBoard::canIssue(unsigned node, uint64_t address, bool persistent) {
    if (node >= nodeCount || lineBusy(address)) return false;
    const auto found = nodes.get().find(address);
    return !persistent || found == nodes.get().end() || !found->second->owner || found->second->owner == node;
}
const LineData& ArchitecturalBoard::expectedDevice(uint64_t address) {
    return deviceExpected[address & ~uint64_t(63)];
}
std::pair<LineData, uint64_t> ArchitecturalBoard::deviceWriteExpectation(uint64_t address) {
    for (const auto& [key, c] : controls)
        if (c.action.kind == TestActionKind::WriteNoSnp && c.action.address == address)
            return {c.expected, c.action.byteEnable};
    throw std::runtime_error("device write lacks independent action intent");
}
void ArchitecturalBoard::issueControl(RequestKey key, const TestAction& action) {
    require(key.node < nodeCount && !controls.count(key) && !pending.haskey(key), "control reused node/TxnID");
    require(action.size <= 6 && !(action.address & ((uint64_t(1) << action.size) - 1)), "control framing invalid");
    const auto kind = action.kind;
    const bool device = kind == TestActionKind::ReadNoSnp || kind == TestActionKind::WriteNoSnp;
    const auto line = action.address & ~uint64_t(63);
    require(!lineBusy(line), "control conflicts with an unfinished line transaction");
    Control control{action};
    if (device) {
        control.expected = expectedDevice(line);
        if (kind == TestActionKind::WriteNoSnp) {
            for (unsigned i = 0; i < 64; ++i) if (action.byteEnable & (uint64_t(1) << i)) control.expected[i] = action.data[i];
            deviceExpected[line] = control.expected; // Independent intended data, never observed DUT data.
        }
    } else {
        require(global.haskey(line) && action.size == 6 && !(action.address & 63), "cache control requires a known full line");
        const auto& state = nodeLine(line);
        if (kind == TestActionKind::MakeUnique)
            require(!state.owner || state.owner == key.node, "MakeUnique would create a second persistent owner");
        if (kind == TestActionKind::Evict)
            require(state.owner == key.node && state.permission != Permission::UD, "Evict needs an owned clean line");
        if (kind == TestActionKind::WriteBack)
            require(state.owner == key.node && state.permission == Permission::UD, "WriteBack needs owned dirty data");
        std::copy_n(global.query(&context, line)->data->data, 64, control.expected.begin());
        if (kind == TestActionKind::WriteBack) require(action.data == control.expected, "WriteBack intent differs from independent latest data");
        lineOwners.emplace(line, key);
        if (kind == TestActionKind::Evict) nodeLine(line) = {}; // Evict is a notification after local discard.
    }
    controls.emplace(key, control);
}
void ArchitecturalBoard::controlDBID(RequestKey key, unsigned dbid, bool traceTag) {
    auto& c = controls.at(key);
    require((c.action.kind == TestActionKind::WriteBack || c.action.kind == TestActionKind::WriteNoSnp) &&
        !c.dbid && dbid < 256 && traceTag == c.action.traceTag, "invalid control DBID response");
    c.dbid = dbid;
}
void ArchitecturalBoard::controlSent(RequestKey key, unsigned beat) {
    auto& c = controls.at(key);
    require(c.dbid && beat < 2 && !(c.sent & (1U << beat)), "control data lacks DBID or duplicates a beat");
    c.sent |= 1U << beat;
    if (c.action.kind == TestActionKind::WriteBack && c.sent == 3) {
        nodeLine(c.action.address) = {};
        if (c.completion) { lineOwners.erase(c.action.address); controls.erase(key); }
    }
}
bool ArchitecturalBoard::controlData(RequestKey key, unsigned beat, unsigned resp, bool traceTag, const uint8_t* bytes) {
    auto& c = controls.at(key);
    require(c.action.kind == TestActionKind::ReadNoSnp && beat < 2 && resp == 0 && traceTag == c.action.traceTag,
        "invalid noncoherent read response");
    const unsigned required = c.action.size == 6 ? 3 : 1U << ((c.action.address >> 5) & 1);
    require((required & (1U << beat)) && !(c.received & (1U << beat)), "noncoherent read beat invalid/duplicate");
    const auto start = unsigned(c.action.address & 63), length = 1U << c.action.size;
    for (unsigned i = beat * 32; i < beat * 32 + 32; ++i)
        if (i >= start && i < start + length) require(bytes[i % 32] == c.expected[i], "noncoherent read data mismatch");
    c.received |= 1U << beat;
    if (c.received != required) return false;
    controls.erase(key);
    return true;
}
void ArchitecturalBoard::controlComplete(RequestKey key, unsigned opcode, unsigned resp, bool traceTag) {
    auto& c = controls.at(key);
    require(!c.completion && traceTag == c.action.traceTag, "control completion duplicate/tag mismatch");
    const auto kind = c.action.kind;
    if (kind == TestActionKind::WriteBack && opcode == 3) {
        require(c.dbid.has_value(), "combined write completion lacks DBID");
        c.completion = true; return; // Completion received; data obligation still owns the entry.
    }
    const bool cmo = kind == TestActionKind::Clean || kind == TestActionKind::Flush || kind == TestActionKind::Invalidate;
    require(opcode == (cmo ? 4U : 1U), "control completion opcode mismatch");
    if (kind == TestActionKind::MakeUnique) {
        require(resp == 2, "MakeUnique grant lacks Unique permission");
        auto& state = nodeLine(c.action.address);
        state.owner = key.node; state.permission = Permission::UC;
        c.completion = true;
        return;
    }
    if (kind == TestActionKind::WriteBack || kind == TestActionKind::WriteNoSnp) {
        const unsigned required = c.action.size == 6 ? 3 : 1U << ((c.action.address >> 5) & 1);
        require(c.sent == required, "write completion preceded all data");
    }
    if (cmo) {
        const auto& state = nodeLine(c.action.address);
        require(kind == TestActionKind::Clean ? state.permission != Permission::UD : !state.owner,
            "CMO completion left an illegal requester state");
        if (kind == TestActionKind::Invalidate) {
            auto entry = std::make_shared<Global_SBEntry>(); entry->status = Global_SBEntry::SB_VALID;
            entry->data = make_shared_tldata<64>();
            const auto& committed = memoryExpected.at(c.action.address);
            std::copy(committed.begin(), committed.end(), entry->data->data);
            global.update(&context, c.action.address, entry);
        }
    } else if (kind == TestActionKind::Evict || kind == TestActionKind::WriteBack) nodeLine(c.action.address) = {};
    lineOwners.erase(c.action.address & ~uint64_t(63));
    controls.erase(key);
}
void ArchitecturalBoard::controlAck(RequestKey key) {
    auto c = controls.at(key);
    require(c.action.kind == TestActionKind::MakeUnique && c.completion, "unexpected control CompAck");
    lineOwners.erase(c.action.address);
    controls.erase(key);
    store(key.node, c.action.address, c.action.data); // Explicit full-line overwrite, not a data upgrade.
}
bool ArchitecturalBoard::idle() {
    return controls.empty() && pending.get().empty() && acks.empty() && lineOwners.empty() &&
           snoops.get().empty() && snoopOwners.empty();
}
} // namespace compactchi
