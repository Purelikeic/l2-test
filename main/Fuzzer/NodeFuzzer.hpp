#pragma once
#include "Fuzzer.h"
#include "../Sequencer/TestNode.hpp"
#include <random>
#include <functional>
#include <optional>

enum class TestScenario { Configured, DeterministicRead, RandomRead, LowerSnoop, Victim, Actions, Replay, RandomActions, ActionReplay };
struct PlannedTestAction { unsigned node; TestAction action; };
class TestScenarioHooks {
public:
    virtual ~TestScenarioHooks() = default;
    virtual bool ready() const = 0;
    virtual void lowerShared(uint64_t) { throw std::logic_error("lower Snoop fixture unavailable"); }
    virtual bool lowerSharedComplete() { return false; }
    virtual void lowerUniqueForward(uint64_t) { throw std::logic_error("forward fixture unavailable"); }
    virtual bool lowerForwardComplete() { return false; }
};
// One action scheduler for configured nodes and finite modes. It contains no
// Compact CHI opcode or TileLink-to-CHI mapping; agents implement the actions.
class NodeFuzzer : public Fuzzer {
    const TLLocalConfig& cfg;
    std::vector<TestNode*> nodes;
    std::vector<unsigned> coherent, transient, mmio;
    TestScenarioHooks& hooks;
    TestScenario scenario;
    std::mt19937 random;
    std::vector<PlannedTestAction> replay;
    unsigned issued = 0, phase = 0;
    unsigned randomEpisode = 0, randomStep = 0;
    std::vector<PlannedTestAction> randomSegment;
    void buildRandomSegment();
    uint64_t anvilU = 0, anvilR = 0, anvilB = 0, anvilEpoch = 0;
    std::vector<unsigned> anvilOrder;
    void unified();
    bool stored = false;
    std::vector<uint64_t> streamNext, streamAt;
    std::function<void(const PlannedTestAction&)> observed;
    std::function<void(const PlannedTestAction&, TestActionResult)> rejected;
    PlannedTestAction next();
    TestActionResult submit(const PlannedTestAction&);
    void configured();
public:
    NodeFuzzer(const TLLocalConfig&, const std::vector<TestNode*>&, TestScenarioHooks&,
               TestScenario, std::vector<PlannedTestAction> replay = {});
    void tick() override;
    bool done() const;
    TestScenario getScenario() const { return scenario; }
    unsigned count() const { return issued; }
    void onRejected(std::function<void(const PlannedTestAction&, TestActionResult)> callback) { rejected = std::move(callback); }
    void onAction(std::function<void(const PlannedTestAction&)> callback) { observed = std::move(callback); }
};
