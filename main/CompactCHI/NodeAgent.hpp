#pragma once
#include "Requester.hpp"
#include <functional>

namespace compactchi {
// The protocol change lives at the node boundary, never in CFuzzer/ReadFuzzer.
class NodeAgent : public TestNode {
    const TLLocalConfig& config;
    Requester& requester;
    ArchitecturalBoard& board;
    std::function<void(uint64_t)> prepare;
public:
    NodeAgent(TestNodeDescriptor descriptor, const TLLocalConfig& config, Requester& requester,
              ArchitecturalBoard& board, std::function<void(uint64_t)> prepare)
        : TestNode(descriptor), config(config), requester(requester), board(board), prepare(std::move(prepare)) {
        if (descriptor.id >= 128) throw std::invalid_argument("configured node ID exceeds Compact source width");
    }
    bool idle() const override { return requester.nodeIdle(descriptor.id); }
    TestActionResult act(const TestAction& action) override {
        const auto kind = action.kind;
        if (kind == TestActionKind::Exclusive || kind == TestActionKind::Atomic || kind == TestActionKind::WriteUnique)
            return TestActionResult::Illegal;
        const bool device = kind == TestActionKind::ReadNoSnp || kind == TestActionKind::WriteNoSnp;
        if (descriptor.kind == TestNodeKind::MMIO ? !device : device) return TestActionResult::Illegal;
        if (descriptor.kind == TestNodeKind::Transient && kind != TestActionKind::ReadOnce) return TestActionResult::Illegal;
        if (action.txn >= 256 || action.size > 6 || (action.address & ((uint64_t(1) << action.size) - 1)))
            return TestActionResult::Illegal;
        const auto begin = device ? config.mmioStart : config.memoryStart;
        const auto end = device ? config.mmioEnd : config.memoryEnd;
        if (!(device ? config.mmioEnable : config.memoryEnable) || action.address < begin ||
            action.address >= end || (uint64_t(1) << action.size) > end - action.address)
            return TestActionResult::Illegal;
        if (!device && (action.size != 6 || (action.address & 63))) return TestActionResult::Illegal;
        if (kind == TestActionKind::MakeUnique && action.byteEnable != ~uint64_t(0)) return TestActionResult::Illegal;
        if (kind == TestActionKind::Clean || kind == TestActionKind::Flush || kind == TestActionKind::Invalidate) {
            if (!config.cmoEnable || action.address < config.cmoStart || action.address >= config.cmoEnd ||
                !(kind == TestActionKind::Clean ? config.cmoEnableCBOClean : kind == TestActionKind::Flush ?
                  config.cmoEnableCBOFlush : config.cmoEnableCBOInval)) return TestActionResult::Illegal;
        }
        if (kind == TestActionKind::WriteNoSnp) {
            const auto offset = action.address & 63;
            const uint64_t allowed = action.size == 6 ? ~uint64_t(0) : ((uint64_t(1) << (1U << action.size)) - 1) << offset;
            if (!action.byteEnable || (action.byteEnable & ~allowed)) return TestActionResult::Illegal;
        }
        if (!device) {
            prepare(action.address);
            const auto& state = board.state(action.address);
            if (kind == TestActionKind::ReadOnce && state.owner == descriptor.id) return TestActionResult::Illegal;
            if (kind == TestActionKind::AcquireShared && state.owner == descriptor.id &&
                state.permission != Permission::SC) return TestActionResult::Illegal;
            if (kind == TestActionKind::Store && (state.owner != descriptor.id ||
                (state.permission != Permission::UC && state.permission != Permission::UD))) return TestActionResult::Illegal;
            if (kind == TestActionKind::Evict && (state.owner != descriptor.id || state.permission == Permission::UD))
                return TestActionResult::Illegal;
            if (kind == TestActionKind::WriteBack && (state.owner != descriptor.id || state.permission != Permission::UD))
                return TestActionResult::Illegal;
        }
        if (!requester.ready(descriptor.id, action)) return TestActionResult::Deferred;
        requester.enqueueAction(descriptor.id, action);
        return TestActionResult::Accepted;
    }
};
} // namespace compactchi
