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
class DelayedNode : public TestNode {
    const uint64_t& cycle;
    std::map<uint64_t,uint64_t> live;
public:
    unsigned peak = 0;
    explicit DelayedNode(TestNodeDescriptor descriptor, const uint64_t& cycle) : TestNode(descriptor), cycle(cycle) {}
    bool idle() const override {
        for (const auto& [a, deadline] : live) if (deadline > cycle) return false;
        return true;
    }
    bool lineIdle(uint64_t address) const override {
        const auto it = live.find(address & ~uint64_t(63));
        return it == live.end() || it->second <= cycle;
    }
    TestActionResult act(const TestAction& a) override {
        for (auto i=live.begin();i!=live.end();) if (i->second<=cycle) i=live.erase(i); else ++i;
        if (live.size() >= 2 || !lineIdle(a.address)) return TestActionResult::Deferred;
        live[a.address & ~uint64_t(63)] = cycle + 17;
        peak = std::max(peak,unsigned(live.size()));
        return TestActionResult::Accepted;
    }
};
class PressureHooks : public TestScenarioHooks {
public:
    bool ready() const override { return false; } // A sustained scheduler must not wait for global ready.
    void lowerShared(uint64_t) override {}
    bool lowerSharedComplete() override { return true; }
    void lowerUniqueForward(uint64_t) override {}
    bool lowerForwardComplete() override { return true; }
};
static void stressVectors(TLLocalConfig config) {
    config.memoryEnd = 0x2000000;
    config.mmioStart = 0x2000000; config.mmioEnd = 0x2010000;
    config.memoryEnable = config.mmioEnable = config.cmoEnable = true;
    config.cmoEnableCBOClean = config.cmoEnableCBOFlush = config.cmoEnableCBOInval = true;
    config.cmoStart = config.memoryStart; config.cmoEnd = config.memoryEnd;
    config.stress.cycles = 200; config.stress.lanes = 4; config.stress.outstanding = 2;
    for (unsigned pattern=0;pattern<3;++pattern) {
        config.stress.pattern = pattern;
        config.sequenceModes.clear();
        if (pattern==1) config.sequenceModes[0]=TLSequenceMode::PASSIVE;
        uint64_t cycle=0; PressureHooks hooks;
        std::vector<std::unique_ptr<DelayedNode>> owned;
        std::vector<TestNode*> views;
        for (const auto d : TestNodeLayout(config)) { owned.push_back(std::make_unique<DelayedNode>(d,cycle)); views.push_back(owned.back().get()); }
        NodeFuzzer fuzzer(config,views,hooks,TestScenario::Stress); fuzzer.set_cycles(&cycle);
        unsigned accepted=0;
        fuzzer.onAction([&](const auto&) { ++accepted; });
        for (;cycle<1000 && !fuzzer.done();++cycle) { fuzzer.tick(); if (cycle<200) check(!fuzzer.done()); }
        check(fuzzer.done() && accepted>50 && cycle>=200 && cycle<1000);
        for (const auto& node : owned) check(node->peak == (pattern==1 && node->descriptor.id==0 ? 0 : 2));
    }
}
int main(int argc, char** argv) {
    check(argc == 2);
    auto config = LoadTestConfig([](TLLocalConfig& c) {
        c.seed = 17; c.coreCount = 2; c.masterCountPerCoreTLC = c.masterCountPerCoreTLUL = 1;
        c.startupCycle = 0; c.memoryStart = 0x1000; c.memoryEnd = 0x40000;
        c.fuzzARIInterval = 10; c.fuzzARITarget = 1;
        c.fuzzStreamStart = 0x1000; c.fuzzStreamEnd = 0x10c0; c.fuzzStreamStep = 64; c.fuzzStreamInterval = 3;
    }, std::string(argv[1]) + "/absent-node-fuzzer.ini");
    stressVectors(config);
    const auto deterministic = run(config, TestScenario::DeterministicRead, true);
    check(deterministic.size() == 4 && deterministic[0].node == 1 && deterministic[1].node == 0);
    auto first = run(config, TestScenario::RandomRead); auto second = run(config, TestScenario::RandomRead);
    check(first.size() == 96 && first.size() == second.size());
    for (unsigned i = 0; i < first.size(); ++i) check(first[i].node == second[i].node && first[i].action.address == second[i].action.address && first[i].action.kind == second[i].action.kind);
    const auto victim = run(config, TestScenario::Victim);
    check(victim.size() == 15); // 14 requests and the independent dirty store.
    unsigned victimRequests = 0;
    for (const auto& p : victim) if (p.action.kind != TestActionKind::Store) {
        const unsigned line = victimRequests < 12 ? victimRequests : victimRequests == 12 ? 0 : 9;
        check(p.action.address == config.memoryStart + uint64_t(line) * 0x20000);
        check(p.action.txn == 20 + victimRequests);
        ++victimRequests;
    }
    check(victimRequests == 14);
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
