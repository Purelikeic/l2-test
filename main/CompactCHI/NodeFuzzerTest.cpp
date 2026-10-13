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
static std::vector<PlannedTestAction> run(TLLocalConfig config, TestScenario scenario, bool defer = false,
    std::vector<PlannedTestAction> replay = {}) {
    std::vector<std::unique_ptr<RecordingNode>> owners;
    std::vector<TestNode*> views;
    for (auto descriptor : TestNodeLayout(config)) { owners.push_back(std::make_unique<RecordingNode>(descriptor)); views.push_back(owners.back().get()); }
    owners[0]->defer = defer;
    Hooks hooks; uint64_t cycle = 0;
    NodeFuzzer fuzzer(config, views, hooks, scenario, std::move(replay)); fuzzer.set_cycles(&cycle);
    std::vector<PlannedTestAction> accepted;
    fuzzer.onAction([&](const auto& planned) { accepted.push_back(planned); });
    for (cycle = 0; cycle < 500 && !fuzzer.done(); ++cycle) fuzzer.tick();
    check(fuzzer.done());
    if (defer) {
        check(owners[0]->calls.size() >= 2);
        const auto& a = owners[0]->calls[0]; const auto& b = owners[0]->calls[1];
        check(a.kind == b.kind && a.address == b.address && a.txn == b.txn && a.traceTag == b.traceTag &&
              a.data == b.data && a.byteEnable == b.byteEnable);
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
    auto actionConfig = config;
    actionConfig.memoryEnable = actionConfig.mmioEnable = actionConfig.cmoEnable = true;
    actionConfig.cmoEnableCBOClean = actionConfig.cmoEnableCBOFlush = actionConfig.cmoEnableCBOInval = true;
    actionConfig.cmoStart = actionConfig.memoryStart; actionConfig.cmoEnd = actionConfig.memoryEnd;
    actionConfig.mmioStart = 0x40000; actionConfig.mmioEnd = 0x41000;
    actionConfig.randomActionEpisodes = 32; actionConfig.randomActionLines = 4;
    const auto mixed = run(actionConfig, TestScenario::RandomActions);
    const auto repeated = run(actionConfig, TestScenario::RandomActions, true);
    const auto replayed = run(actionConfig, TestScenario::ActionReplay, true, mixed);
    check(replayed.size()==mixed.size());
    for (unsigned i=0;i<mixed.size();++i) {
        const auto& a=mixed[i]; const auto& b=replayed[i];
        check(a.node==b.node && a.action.kind==b.action.kind && a.action.address==b.action.address &&
            a.action.txn==b.action.txn && a.action.traceTag==b.action.traceTag && a.action.size==b.action.size &&
            a.action.byteEnable==b.action.byteEnable && a.action.data==b.action.data);
    }
    check(mixed.size() == repeated.size() && mixed.size() >= 32 && mixed.size() <= 128);
    const auto layout = TestNodeLayout(actionConfig);
    std::map<uint64_t, unsigned> owned;
    for (unsigned i=0; i<mixed.size(); ++i) {
        const auto& a = mixed[i]; const auto& b = repeated[i];
        check(a.node == b.node && a.action.kind == b.action.kind && a.action.address == b.action.address &&
              a.action.txn == b.action.txn && a.action.traceTag == b.action.traceTag &&
              a.action.data == b.action.data && a.action.byteEnable == b.action.byteEnable);
        check(a.action.txn < 256 && a.action.kind != TestActionKind::MakeUnique);
        const auto k=a.action.kind; const auto address=a.action.address;
        const auto role=layout[a.node].kind;
        if (role == TestNodeKind::MMIO) {
            check(k == TestActionKind::ReadNoSnp || k == TestActionKind::WriteNoSnp);
            check(a.action.size == 3 && address >= actionConfig.mmioStart && address < actionConfig.mmioStart + 4*64);
        } else {
            check(address >= actionConfig.memoryStart && address < actionConfig.memoryStart + 4*64 && !(address & 63));
            if (role == TestNodeKind::Transient) check(k == TestActionKind::ReadOnce);
            else if (k == TestActionKind::AcquireShared || k == TestActionKind::AcquireUnique) {
                check(!owned.count(address)); owned[address]=a.node;
            } else {
                check(owned.count(address) && owned.at(address)==a.node);
                if (k == TestActionKind::Evict || k == TestActionKind::WriteBack ||
                    k == TestActionKind::Flush || k == TestActionKind::Invalidate) owned.erase(address);
            }
        }
    }
    check(owned.empty());
    actionConfig.sequenceModes[0] = TLSequenceMode::RANDOM_ACTIONS;
    check(run(actionConfig, TestScenario::Configured).size() == mixed.size());
    actionConfig.randomActionEpisodes=41;
    bool invalid=false;
    try { run(actionConfig, TestScenario::RandomActions); }
    catch (const std::invalid_argument&) { invalid=true; }
    check(invalid);
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
    std::puts("protocol-neutral node fuzzer: configured modes, bounded random segments, seed/deferred replay, legal leases and budget rejection passed");
}
