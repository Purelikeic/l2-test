#include "../Sequencer/TLSequencer.hpp"
#include "../System/TLConfig.hpp"
#include "NodeAgent.hpp"
#include <fstream>
#include <cstdio>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Factory : public TestProtocolAdapter, public TestScenarioHooks, public TLLocalContext {
    TLLocalConfig* cfg = nullptr;
    uint64_t* clocks = nullptr;
    GlobalBoard<paddr_t>* global = nullptr;
public:
    std::unique_ptr<compactchi::ArchitecturalBoard> board;
    std::unique_ptr<compactchi::Requester> requester;
    unsigned created = 0;
    uint64_t cycle() const noexcept override { return *clocks; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return cfg->seed; }
    const TLLocalConfig& config() const noexcept override { return *cfg; }
    TLLocalConfig& config() noexcept override { return *cfg; }
    bool ready() const override { return requester->idle() && board->idle(); }
    bool hasActionNodes() const override { return true; }
    void initialize(TLLocalConfig& config, uint64_t& cycles, GlobalBoard<paddr_t>& global,
                    UncachedBoard<paddr_t>& uncached, axi_agent::MemoryAgent&,
                    axi_agent::Bundle<BEATSIZE_MEMORY>&) override {
        cfg = &config; clocks = &cycles; this->global = &global;
        const auto layout = TestNodeLayout(config);
        board = std::make_unique<compactchi::ArchitecturalBoard>(*this, layout.size(), global, uncached);
        requester = std::make_unique<compactchi::Requester>(*board, 1);
        requester->configureNodes(layout);
    }
    std::unique_ptr<TestNode> createNode(TestNodeDescriptor d) override {
        ++created;
        return std::make_unique<compactchi::NodeAgent>(d, *cfg, *requester, *board, [this](uint64_t address) {
            if (!global->haskey(address)) {
                compactchi::LineData bytes{};
                for (unsigned i = 0; i < 64; ++i) bytes[i] = uint8_t(i ^ cfg->seed);
                board->initialize(address, bytes);
            }
        });
    }
    std::unique_ptr<NodeFuzzer> createFuzzer(const TLLocalConfig& c, const std::vector<TestNode*>& nodes) override {
        return std::make_unique<NodeFuzzer>(c, nodes, *this, TestScenario::Configured);
    }
    void handleChannels() override {}
    void fuzz() override {}
    void updateSignals() override {}
    void finalize() override { requester.reset(); board.reset(); }
    void grant() {
        const auto req = requester->request();
        requester->requestAccepted();
        const auto bytes = global->query(this, req.address)->data;
        for (unsigned beat = 0; beat < 2; ++beat) {
            compactchi::DataFlit data{1, req.node, req.txn, 7, beat, req.opcode == 3 ? 0U : 2U, req.traceTag};
            std::copy_n(bytes->data + beat * 32, 32, data.data.begin()); requester->receiveData(data);
        }
        if (req.opcode != 3) requester->responseAccepted();
    }
};
int main(int argc, char** argv) {
    check(argc == 2, "test output path missing");
    const auto file = std::string(argv[1]) + "/node-config.ini";
    for (const auto shape : {std::array<unsigned, 3>{1, 2, 1}, {2, 1, 2}, {3, 0, 1}}) {
        std::ofstream ini(file);
        ini << "[tltest.config]\ncore=" << shape[0] << "\ncore.tl_c=" << shape[1] << "\ncore.tl_ul=" << shape[2]
            << "\nmemory.start=4096\nmemory.end=8192\nmemory.backend.port.count=1\n";
        ini.close();
        auto config = LoadTestConfig([](TLLocalConfig& c) { c.seed = 17; }, file);
        const auto layout = TestNodeLayout(config);
        check(layout.size() == shape[0] * (shape[1] + shape[2] + 1), "INI topology count mismatch");
        for (const auto& node : layout) config.sequenceModes[node.id] = TLSequenceMode::PASSIVE;
        Factory factory; TLSequencer sequencer;
        sequencer.Initialize(config, factory);
        check(sequencer.IsAlive() && factory.created == layout.size() && sequencer.GetActionNodes().size() == layout.size(),
            "Sequencer did not create every configured action node");
        sequencer.Tick(10); sequencer.Tock(); check(sequencer.IsAlive(), "passive configured node tick failed");
        sequencer.Finalize();
        // The same INI must also create the original TL cache/MMIO topology.
        TLSequencer legacy;
        legacy.Initialize(config);
        check(legacy.IsAlive() && legacy.GetAgentCount() == shape[0] * (shape[1] + shape[2]), "legacy topology differs");
        for (unsigned core = 0; core < shape[0]; ++core) check(!legacy.MMIO(core).a.valid, "MMIO per-core allocation wrong");
        legacy.Tick(1); legacy.Tock(); check(legacy.IsAlive(), "legacy passive topology tick failed");
        legacy.Finalize();
    }
    auto config = LoadTestConfig([](TLLocalConfig& c) {
        c.seed = 17; c.coreCount = 2; c.masterCountPerCoreTLC = c.masterCountPerCoreTLUL = 1;
        c.memoryStart = 0x1000; c.memoryEnd = 0x2000; c.mmioEnable = 1;
        c.mmioStart = 0x1000; c.mmioEnd = 0x2000; // Ownership policy must not affect MMIO.
        c.cmoEnable = 1; c.cmoStart = 0x1000; c.cmoEnd = 0x2000;
    }, std::string(argv[1]) + "/absent-node-config.ini");
    Factory factory; TLSequencer sequencer;
    for (unsigned i = 0; i < 6; ++i) config.sequenceModes[i] = TLSequenceMode::PASSIVE;
    sequencer.Initialize(config, factory); check(sequencer.IsAlive(), "node factory failed");
    auto& nodes = sequencer.GetActionNodes();
    TestAction unique{TestActionKind::AcquireUnique, 0x1000, 20, true};
    check(nodes[0]->act(unique) == TestActionResult::Accepted, "coherent acquire rejected");
    check(nodes[2]->act(unique) == TestActionResult::Deferred, "offered persistent owner was not reserved");
    factory.grant();
    check(nodes[2]->act(unique) == TestActionResult::Deferred, "second persistent owner accepted");
    TestAction once{TestActionKind::ReadOnce, 0x1000, 21, false};
    check(nodes[1]->act(once) == TestActionResult::Accepted, "ReadOnce was blocked by persistent owner");
    factory.grant();
    check(factory.board->state(0x1000).owner == 0, "ReadOnce entered or erased persistent ownership");
    TestAction device{TestActionKind::ReadNoSnp, 0x1000, 22, true, 3};
    check(nodes[4]->act(device) == TestActionResult::Accepted, "MMIO was blocked by cache ownership");
    factory.requester->requestAccepted();
    compactchi::DataFlit data{1, 4, 22, 0, 0, 0, true};
    factory.requester->receiveData(data);
    check(factory.board->state(0x1000).owner == 0, "MMIO entered or erased persistent ownership");
    for (auto kind : {TestActionKind::Exclusive, TestActionKind::Atomic, TestActionKind::WriteUnique})
        for (const auto& node : nodes) check(node->act({kind, 0x1000}) == TestActionResult::Illegal, "unsupported action was approximated");
    check(nodes[1]->act(unique) == TestActionResult::Illegal && nodes[4]->act(unique) == TestActionResult::Illegal,
        "node protocol capability matrix ignored");
    TestAction store{TestActionKind::Store, 0x1000}; store.data.fill(0x5a);
    check(nodes[0]->act(store) == TestActionResult::Accepted, "legal local store failed");
    TestAction wb{TestActionKind::WriteBack, 0x1000, 23, false};
    check(nodes[0]->act(wb) == TestActionResult::Accepted, "dirty release failed");
    factory.requester->eventAccepted(); check(!factory.requester->snoopReady(), "WriteBack replied before DBID");
    factory.requester->receiveResponse({1, 0, 23, 3, 0, false}, 11, 0);
    for (unsigned beat = 0; beat < 2; ++beat) {
        check(!factory.requester->idle(), "combined completion retired before data");
        const auto& flit = factory.requester->data();
        check(flit.opcode == 2 && flit.txn == 11 && flit.dataId == beat && flit.data[0] == 0x5a,
            "release did not use dirty snapshot/DBID");
        factory.requester->dataAccepted();
    }
    check(factory.requester->idle() && !factory.board->state(0x1000).owner, "writeback owner did not retire");
    TestAction overwrite{TestActionKind::MakeUnique, 0x1000, 24, true}; overwrite.data.fill(0xa7);
    check(nodes[0]->act(overwrite) == TestActionResult::Accepted, "full overwrite permission rejected");
    factory.requester->requestAccepted(); factory.requester->receiveResponse({1, 0, 24, 1, 2, true}, 12, 0);
    check(!factory.requester->idle(), "MakeUnique retired before CompAck");
    factory.requester->responseAccepted();
    check(factory.board->state(0x1000).permission == compactchi::Permission::UD, "MakeUnique did not perform explicit overwrite");
    TestAction clean{TestActionKind::Clean, 0x1000, 25, false};
    check(nodes[0]->act(clean) == TestActionResult::Accepted, "CMO action failed");
    factory.requester->requestAccepted(); factory.requester->receiveSnoop({1, 0, 6, 2, 0x1000, true});
    factory.requester->dataAccepted(); factory.requester->dataAccepted();
    factory.requester->receiveResponse({1, 0, 25, 4, 0, false}, 0, 0);
    check(factory.board->state(0x1000).permission != compactchi::Permission::UD, "CleanShared kept dirty owner");
    TestAction evict{TestActionKind::Evict, 0x1000, 26, true};
    check(nodes[0]->act(evict) == TestActionResult::Accepted, "clean release failed");
    factory.requester->eventAccepted(); factory.requester->receiveResponse({1, 0, 26, 1, 0, true}, 0, 0);
    check(nodes[2]->act(unique) == TestActionResult::Accepted, "release did not unblock another coherent identity"); factory.grant();
    sequencer.Finalize();
    std::puts("configured Sequencer nodes: INI topology, role actions, queued/stable single-owner gating, transient/MMIO exemption and completion obligations passed");
}
