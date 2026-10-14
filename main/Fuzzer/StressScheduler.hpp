#pragma once
#include "NodeFuzzer.hpp"
#include <optional>

// Protocol-neutral sustained workers. Only endpoint completion controls a
// worker's next action; no global drain or observed DUT cache state is used.
class StressScheduler {
    struct Worker {
        TestNode* node;
        unsigned slot;
        uint64_t address = 0, epoch = 0;
        std::vector<TestActionKind> segment;
        unsigned step = 0;
        std::optional<TestAction> offered;
        unsigned snoopStep = 0;
        bool special = false;
    };
    const TLLocalConfig& cfg;
    TestScenarioHooks& hooks;
    std::vector<Worker> workers;
    std::vector<unsigned> nextTxn;
    std::mt19937 random;
    uint64_t end = 0;
    unsigned slots, cursor = 0;
    bool started = false;

    uint64_t address(const Worker& w) const {
        const bool device = w.node->descriptor.kind == TestNodeKind::MMIO;
        const uint64_t begin = device ? cfg.mmioStart : cfg.memoryStart;
        if (device || cfg.stress.pattern == 2) return begin + uint64_t(w.slot) * 64;
        const uint64_t stride = cfg.stress.pattern == 1 ? 0x20000 : 64;
        const uint64_t count = ((cfg.memoryEnd - cfg.memoryStart) / stride / slots) * slots;
        return begin + ((w.epoch * slots + w.slot) % count) * stride;
    }
    std::vector<TestActionKind> segment(const Worker& w) const {
        using K = TestActionKind;
        const auto kind = w.node->descriptor.kind;
        if (kind == TestNodeKind::MMIO) return {K::WriteNoSnp, K::ReadNoSnp};
        if (cfg.stress.pattern == 1 || kind == TestNodeKind::Transient) return {K::ReadOnce};
        if (cfg.stress.pattern == 0) return {K::AcquireShared, K::Evict};
        if (w.epoch == 0) return {K::AcquireUnique, K::Store, K::WriteBack};
        switch (w.slot % 4) {
        case 0: return {K::AcquireUnique, K::Store, K::WriteBack};
        case 1: return {K::AcquireUnique, K::Store, K::Clean, K::Evict};
        case 2: return {K::AcquireUnique, K::Store, K::Flush};
        default: return {K::AcquireUnique, K::Store, K::Invalidate};
        }
    }
public:
    StressScheduler(const TLLocalConfig& cfg, const std::vector<TestNode*>& nodes, TestScenarioHooks& hooks)
        : cfg(cfg), hooks(hooks), nextTxn(nodes.size(), 0), random(cfg.seed ^ 0x57e55),
          slots(nodes.size() * cfg.stress.lanes) {
        if (!cfg.stress.cycles || cfg.stress.cycles > 1000000000 || cfg.stress.lanes < 2 || cfg.stress.lanes > 32 ||
            cfg.stress.outstanding < 2 || cfg.stress.outstanding > 64 || cfg.stress.pattern > 2 ||
            slots == 0 || cfg.memoryEnd <= cfg.memoryStart || cfg.mmioEnd <= cfg.mmioStart ||
            !cfg.memoryEnable || !cfg.mmioEnable || (cfg.memoryStart & 63) || (cfg.mmioStart & 63))
            throw std::invalid_argument("invalid sustained stress budget/window/regions");
        const uint64_t stride = cfg.stress.pattern == 1 ? 0x20000 : 64;
        if (cfg.memoryEnd - cfg.memoryStart < uint64_t(slots) * stride ||
            cfg.mmioEnd - cfg.mmioStart < uint64_t(slots) * 64)
            throw std::invalid_argument("stress working set does not fit configured memory/MMIO");
        bool special = false;
        for (auto* node : nodes) for (unsigned lane = 0; lane < cfg.stress.lanes; ++lane) {
            const auto mode=cfg.sequenceModes.find(node->descriptor.id);
            if (mode!=cfg.sequenceModes.end() && mode->second==TLSequenceMode::PASSIVE) continue;
            Worker w{node, node->descriptor.id * cfg.stress.lanes + lane};
            if (cfg.stress.pattern == 2 && !special && node->descriptor.kind == TestNodeKind::Coherent && lane == 1) {
                w.special = true; special = true;
                // Existing lower-Snoop fixture reserves memoryStart+64.
                w.address = cfg.memoryStart + 64;
            }
            workers.push_back(w);
        }
        if (workers.empty()) throw std::invalid_argument("stress has no active nodes");
        if (cfg.stress.pattern == 2 && (!special || !cfg.cmoEnable || !cfg.cmoEnableCBOClean ||
                !cfg.cmoEnableCBOFlush || !cfg.cmoEnableCBOInval))
            throw std::invalid_argument("mixed stress needs coherent nodes and enabled CMOs");
    }
    void tick(uint64_t cycle, const std::function<TestActionResult(const PlannedTestAction&)>& submit) {
        if (!started) { end = cycle + cfg.stress.cycles; started = true; }
        for (unsigned i=0;i<workers.size();++i) {
            auto& w = workers[(cursor+i) % workers.size()];
            if (w.special) {
                if (!w.node->lineIdle(w.address)) continue;
                if (w.snoopStep == 2) { hooks.lowerShared(w.address); ++w.snoopStep; continue; }
                if (w.snoopStep == 3) { if (hooks.lowerSharedComplete()) ++w.snoopStep; continue; }
                if (w.snoopStep == 4) { hooks.lowerUniqueForward(w.address); ++w.snoopStep; continue; }
                if (w.snoopStep == 5) { if (hooks.lowerForwardComplete()) ++w.snoopStep; continue; }
                if (w.snoopStep == 6) continue;
                if (w.segment.empty()) w.segment = {TestActionKind::AcquireUnique, TestActionKind::Store};
            } else {
                if (w.segment.empty()) {
                    if (cycle >= end) continue;
                    w.address = address(w); w.segment = segment(w); w.step = 0;
                }
                if (!w.node->lineIdle(w.address)) continue;
            }
            const auto node = w.node->descriptor.id;
            if (!w.offered) {
                TestAction a{w.segment[w.step], w.address, nextTxn[node], bool(random() & 1)};
                for (auto& b : a.data) b = uint8_t(random());
                if (w.node->descriptor.kind == TestNodeKind::MMIO) {
                    const unsigned offset = 8 * (w.slot % 8);
                    a.address += offset; a.size = 3;
                    a.byteEnable = uint64_t(1 + random() % 255) << offset;
                }
                w.offered = a;
            }
            const auto result = submit({node, *w.offered});
            if (result != TestActionResult::Accepted) continue;
            nextTxn[node] = (nextTxn[node] + 1) % 256;
            w.offered.reset();
            if (w.special) ++w.snoopStep;
            if (++w.step == w.segment.size()) {
                w.segment.clear(); w.step = 0; ++w.epoch;
            }
        }
        cursor = (cursor + 1) % workers.size();
    }
    bool done(uint64_t cycle) const {
        if (!started || cycle < end) return false;
        for (const auto& w : workers) {
            if ((w.special && w.snoopStep != 6) || !w.segment.empty() || w.offered ||
                !w.node->lineIdle(w.address)) return false;
        }
        return true;
    }
};
