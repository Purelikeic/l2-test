#include "../Fuzzer/NodeFuzzer.hpp"
#include "../System/TLConfig.hpp"
#include <cstdio>
#include <algorithm>

static void check(bool value) { if (!value) throw std::runtime_error("generic action scheduler vector failed"); }
class RecordingNode : public TestNode {
public:
    std::vector<TestAction> calls;
    bool defer = false;
    explicit RecordingNode(TestNodeDescriptor descriptor) : TestNode(descriptor) {}
    bool idle() const override { return true; }
    TestActionResult act(const TestAction& action) override {
        calls.push_back(action);
        if (defer) { defer = false; return TestActionResult::Deferred; }
        return TestActionResult::Accepted;
    }
};
class Hooks : public TestScenarioHooks {
public:
    bool ready() const override { return true; }
};
static std::vector<PlannedTestAction> run(TLLocalConfig config, TestScenario scenario, bool defer = false) {
    std::vector<std::unique_ptr<RecordingNode>> owners;
    std::vector<TestNode*> views;
    for (auto descriptor : TestNodeLayout(config)) { owners.push_back(std::make_unique<RecordingNode>(descriptor)); views.push_back(owners.back().get()); }
    owners[0]->defer = defer;
    Hooks hooks; uint64_t cycle = 0;
    NodeFuzzer fuzzer(config, views, hooks, scenario); fuzzer.set_cycles(&cycle);
    std::vector<PlannedTestAction> accepted;
    fuzzer.onAction([&](const auto& planned) { accepted.push_back(planned); });
    for (cycle = 0; cycle < 500 && !fuzzer.done(); ++cycle) fuzzer.tick();
    check(fuzzer.done());
    if (defer) {
        check(owners[0]->calls.size() >= 2);
        const auto& a = owners[0]->calls[0]; const auto& b = owners[0]->calls[1];
        check(a.kind == b.kind && a.address == b.address && a.txn == b.txn && a.traceTag == b.traceTag);
    }
    return accepted;
}
int main(int argc, char** argv) {
    check(argc == 2);
    auto config = LoadTestConfig([](TLLocalConfig& c) {
        c.seed = 17; c.coreCount = 2; c.masterCountPerCoreTLC = c.masterCountPerCoreTLUL = 1;
        c.startupCycle = 0; c.memoryStart = 0x1000; c.memoryEnd = 0x40000;
        c.fuzzARIInterval = 10; c.fuzzARITarget = 1;
        c.fuzzStreamStart = 0x1000; c.fuzzStreamEnd = 0x10c0; c.fuzzStreamStep = 64; c.fuzzStreamInterval = 3;
    }, std::string(argv[1]) + "/absent-node-fuzzer.ini");
    const auto deterministic = run(config, TestScenario::DeterministicRead, true);
    check(deterministic.size() == 4 && deterministic[0].node == 1 && deterministic[1].node == 0);
    auto first = run(config, TestScenario::RandomRead); auto second = run(config, TestScenario::RandomRead);
    check(first.size() == 96 && first.size() == second.size());
    for (unsigned i = 0; i < first.size(); ++i) check(first[i].node == second[i].node && first[i].action.address == second[i].action.address && first[i].action.kind == second[i].action.kind);
    for (const auto node : TestNodeLayout(config)) config.sequenceModes[node.id] = TLSequenceMode::PASSIVE;
    config.sequenceModes[0] = TLSequenceMode::FUZZ_STREAM;
    const auto stream = run(config, TestScenario::Configured);
    check(stream.size() == 3);
    for (unsigned i = 0; i < 3; ++i) check(stream[i].action.address == config.fuzzStreamStart + i * config.fuzzStreamStep);
    config.sequenceModes[0] = TLSequenceMode::FUZZ_ARI;
    const auto ari = run(config, TestScenario::Configured);
    check(!ari.empty());
    for (const auto& planned : ari) check(planned.node == 0 && planned.action.address >= config.memoryStart && planned.action.address < config.memoryEnd && !(planned.action.address & 63));
    config.sequenceModes.clear(); config.sequenceModes[0] = TLSequenceMode::DETERMINISTIC_READ;
    check(run(config, TestScenario::Configured).size() == 4);
    config.sequenceModes.clear();
    config.unifiedSequenceEnable = true; config.unifiedSequenceMode = TLUnifiedSequenceMode::ANVIL;
    config.unifiedSequenceModeAnvil.size = 8; config.unifiedSequenceModeAnvil.widthB = 1;
    config.unifiedSequenceModeAnvil.epoch = 1;
    const auto anvil = run(config, TestScenario::Configured);
    check(anvil.size() == 24);
    check(std::count_if(anvil.begin(), anvil.end(), [](const auto& p) { return p.action.kind == TestActionKind::MakeUnique; }) == 8);
    check(std::count_if(anvil.begin(), anvil.end(), [](const auto& p) { return p.action.kind == TestActionKind::WriteBack; }) == 8);
    std::puts("protocol-neutral node fuzzer: configured identities, seed replay, stable deferred actions, ARI/stream and INI finite modes passed");
}
