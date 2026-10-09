#include "CHIHomeMemory.hpp"
#include <algorithm>
#include <stdexcept>

namespace chi_agent {
HomeMemory::HomeMemory(TLLocalConfig& cfg, uint64_t& cycle)
    : config(cfg), cycles(cycle), memory(&cfg, cfg.seed, &cycle, 0) {
    if (cfg.memoryEnd <= cfg.memoryStart || cfg.memoryCycleUnit == 0)
        throw std::invalid_argument("CHI Home requires a nonempty memory region and cycle unit");
    memory.connect(&port);
    memory.update_signal();
}

bool HomeMemory::ready(bool write) const {
    const auto depth = write ? config.memoryWriteDepth : config.memoryReadDepth;
    // Completed responses retain capacity until the CHI Home takes ownership.
    const size_t active = write ? writes.size() : reads.size();
    size_t held = 0;
    for (const auto& result : completed) if (result.write == write) ++held;
    return depth == 0 || active + held < depth;
}

void HomeMemory::submit(const MemoryRequest& request) {
    if (!ready(request.write)) throw std::runtime_error("CHI Home memory capacity exceeded");
    if ((request.address & (DATASIZE - 1)) || !memory.accessible(request.address) ||
        !memory.accessible(request.address + DATASIZE - 1))
        throw std::invalid_argument("CHI Home memory request must address a complete aligned line");
    if (request.write) {
        if (!writes.emplace(request.id, request.address).second)
            throw std::runtime_error("CHI Home reused an active memory write ID");
        writeQueue.push_back({request});
    } else {
        if (!reads.emplace(request.id, PendingRead{request.address}).second)
            throw std::runtime_error("CHI Home reused an active memory read ID");
        readQueue.push_back(request);
    }
}

void HomeMemory::drive() {
    port.ar.valid = !readQueue.empty();
    if (port.ar.valid) {
        const auto& request = readQueue.front();
        port.ar.id = request.id;
        port.ar.addr = request.address;
        port.ar.size = 5;
        port.ar.len = 1;
        port.ar.burst = axi_agent::BURST_INCR;
    }
    port.aw.valid = !writeQueue.empty() && !writeQueue.front().addressSent;
    port.w.valid = !writeQueue.empty() && writeQueue.front().addressSent;
    if (!writeQueue.empty()) {
        const auto& pending = writeQueue.front();
        port.aw.id = pending.request.id;
        port.aw.addr = pending.request.address;
        port.aw.size = 5;
        port.aw.len = 1;
        port.aw.burst = axi_agent::BURST_INCR;
        if (port.w.valid) {
            port.w.last = pending.beat == 1;
            port.w.strb = (pending.request.byteEnable >> (pending.beat * 32)) & 0xffffffffULL;
            port.w.data = make_shared_tldata<BEATSIZE_MEMORY>();
            std::copy_n(pending.request.data.begin() + pending.beat * 32, 32, port.w.data->data);
        }
    }
    // Capacity is reserved at submit, so response capture never waits for a
    // downstream CHI consumer or a new requester operation.
    port.r.ready = 1;
    port.b.ready = 1;
}

void HomeMemory::tick() {
    drive();
    const bool arFire = port.ar.fire(), awFire = port.aw.fire(), wFire = port.w.fire();
    if (port.r.fire()) {
        auto it = reads.find(port.r.id);
        if (it == reads.end()) throw std::runtime_error("unknown CHI Home memory read response");
        auto& pending = it->second;
        if (pending.beats >= 2 || port.r.last != (pending.beats == 1) || port.r.resp != 0)
            throw std::runtime_error("invalid CHI Home memory read response framing");
        std::copy_n(port.r.data->data, 32, pending.data.begin() + pending.beats++ * 32);
        if (port.r.last) {
            completed.push_back({port.r.id, pending.address, false, pending.data});
            reads.erase(it);
        }
    }
    if (port.b.fire()) {
        auto it = writes.find(port.b.id);
        if (it == writes.end() || port.b.resp != 0)
            throw std::runtime_error("invalid CHI Home memory write response");
        completed.push_back({port.b.id, it->second, true});
        writes.erase(it);
    }
    memory.handle_channel();
    if (arFire) readQueue.pop_front();
    if (awFire) writeQueue.front().addressSent = true;
    if (wFire && ++writeQueue.front().beat == 2) writeQueue.pop_front();
    memory.update_signal();
}

void HomeMemory::popCompletion() {
    if (completed.empty()) throw std::runtime_error("CHI Home consumed an absent memory completion");
    completed.pop_front();
}
bool HomeMemory::idle() const {
    return readQueue.empty() && writeQueue.empty() && reads.empty() && writes.empty() && completed.empty();
}
} // namespace chi_agent
