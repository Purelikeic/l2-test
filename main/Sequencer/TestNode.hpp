#pragma once

#include "../Base/TLLocal.hpp"
#include <array>
#include <memory>
#include <vector>
#include <stdexcept>
#include <limits>

// Actions and topology have no wire opcode or protocol-specific fuzzer branch.
enum class TestNodeKind { Coherent, Transient, MMIO };
enum class TestActionKind {
    AcquireShared, AcquireUnique, MakeUnique, Evict, WriteBack,
    Clean, Flush, Invalidate, ReadOnce, ReadNoSnp, WriteNoSnp, Store,
    Exclusive, Atomic, WriteUnique
};
enum class TestActionResult { Accepted, Deferred, Illegal };
struct TestNodeDescriptor { unsigned id, core, slot; TestNodeKind kind; };
struct TestAction {
    TestActionKind kind;
    uint64_t address;
    unsigned txn = 0;
    bool traceTag = false;
    unsigned size = 6;
    std::array<uint8_t, 64> data{};
    uint64_t byteEnable = ~uint64_t(0);
};
class TestNode {
public:
    const TestNodeDescriptor descriptor;
    explicit TestNode(TestNodeDescriptor d) : descriptor(d) {}
    virtual ~TestNode() = default;
    virtual TestActionResult act(const TestAction& action) = 0;
    virtual bool idle() const = 0;
    virtual bool lineIdle(uint64_t) const { return idle(); }
};
inline std::vector<TestNodeDescriptor> TestNodeLayout(const TLLocalConfig& config) {
    const uint64_t cache = uint64_t(config.coreCount) *
        (uint64_t(config.masterCountPerCoreTLC) + config.masterCountPerCoreTLUL);
    if (cache + config.coreCount > std::numeric_limits<unsigned>::max())
        throw std::invalid_argument("configured node identities overflow");
    std::vector<TestNodeDescriptor> nodes;
    unsigned id = 0;
    for (unsigned core = 0; core < config.coreCount; ++core) {
        for (unsigned slot = 0; slot < config.masterCountPerCoreTLC; ++slot)
            nodes.push_back({id++, core, slot, TestNodeKind::Coherent});
        for (unsigned slot = 0; slot < config.masterCountPerCoreTLUL; ++slot)
            nodes.push_back({id++, core, slot, TestNodeKind::Transient});
    }
    // TL's MMIO IDs are in a separate port namespace. A shared wire protocol
    // gives them an explicit non-overlapping region without changing the INI.
    for (unsigned core = 0; core < config.coreCount; ++core)
        nodes.push_back({id++, core, 0, TestNodeKind::MMIO});
    return nodes;
}
