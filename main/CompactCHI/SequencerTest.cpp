#include "../Sequencer/TLSequencer.hpp"
#include "../System/TLConfig.hpp"
#include "../Events/TLSystemEvent.hpp"
#include "../Memory/CHIHomeMemory.hpp"
#include "ArchitecturalBoard.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class Adapter : public TestProtocolAdapter {
    axi_agent::Bundle<BEATSIZE_MEMORY>* port = nullptr;
    uint64_t* cycles = nullptr;
    unsigned phase = 0;
    unsigned received = 0;
    bool sent = false;
public:
    bool finalized = false;
    bool failCycle = false;
    void initialize(TLLocalConfig& config, uint64_t& cycle, GlobalBoard<paddr_t>& global,
                    UncachedBoard<paddr_t>& uncached, axi_agent::MemoryAgent& memory,
                    axi_agent::Bundle<BEATSIZE_MEMORY>& memoryPort) override {
        require(config.seed == 17 && memory.sysSeed() == 17, "Sequencer did not preserve seed");
        cycles = &cycle;
        port = &memoryPort;
        require(global.get().empty(), "Sequencer provided a nonempty initial GlobalBoard");
        for (unsigned i = 0; i < 64; ++i) memory.access(config.memoryStart + i) = uint8_t(i ^ 17);
        require(!uncached.haskey(0, config.memoryStart, 1), "Sequencer uncached board was not initialized");
    }
    void handleChannels() override {
        require(phase == 0 && cycles, "Sequencer channel phase order mismatch");
        if (failCycle) throw std::runtime_error("deliberate adapter failure");
        if (port->ar.fire()) sent = true;
        if (port->r.fire()) {
            require(port->r.id == 3 && port->r.last == (received == 1), "Sequencer memory framing mismatch");
            for (unsigned i = 0; i < 32; ++i)
                require(port->r.data->data[i] == uint8_t((received * 32 + i) ^ 17),
                        "Sequencer memory data mismatch");
            ++received;
        }
        phase = 1;
    }
    void fuzz() override {
        require(phase == 1, "Sequencer fuzzer phase order mismatch");
        phase = 2;
    }
    void updateSignals() override {
        require(phase == 2, "Sequencer signal phase order mismatch");
        port->ar.valid = !sent;
        port->ar.id = 3;
        port->ar.addr = 0x1000;
        port->ar.size = 5;
        port->ar.len = 1;
        port->ar.burst = axi_agent::BURST_INCR;
        port->r.ready = *cycles >= 20;
        phase = 0;
    }
    void finalize() override {
        if (!failCycle) require(received == 2, "Sequencer did not drain the memory transaction");
        finalized = true;
    }
};
class HomeAdapter : public TestProtocolAdapter, public TLLocalContext {
    TLLocalConfig* cfg = nullptr;
    uint64_t* cycleValue = nullptr;
    std::unique_ptr<chi_agent::HomeMemory> home;
    std::unique_ptr<compactchi::ArchitecturalBoard> board;
    compactchi::LineData expected{};
    class ReadFuzzer : public Fuzzer {
        HomeAdapter& owner;
        bool issued = false;
    public:
        explicit ReadFuzzer(HomeAdapter& owner) : owner(owner) {}
        void tick() override {
            if (!issued && *cycles >= 10) {
                owner.board->issue({0, 15}, compactchi::ReadClass::Once, 0x1000, true);
                owner.home->submit({3, 0x1000, false});
                issued = true;
            }
        }
    };
    ReadFuzzer fuzzer{*this};
    bool done = false;
public:
    bool finalized = false;
    uint64_t cycle() const noexcept override { return *cycleValue; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return cfg->seed; }
    const TLLocalConfig& config() const noexcept override { return *cfg; }
    TLLocalConfig& config() noexcept override { return *cfg; }
    void initialize(TLLocalConfig& config, uint64_t& cycles, GlobalBoard<paddr_t>& global,
                    UncachedBoard<paddr_t>& uncached, axi_agent::MemoryAgent& memory,
                    axi_agent::Bundle<BEATSIZE_MEMORY>& port) override {
        cfg = &config;
        cycleValue = &cycles;
        fuzzer.set_cycles(&cycles);
        home = std::make_unique<chi_agent::HomeMemory>(config, cycles, memory, port);
        board = std::make_unique<compactchi::ArchitecturalBoard>(*this, config.GetAgentCount(), global, uncached);
        for (unsigned i = 0; i < 64; ++i) {
            expected[i] = uint8_t(i ^ 17);
            memory.access(0x1000 + i) = expected[i];
        }
        board->initialize(0x1000, expected);
        require(global.haskey(0x1000), "architectural board did not use the Sequencer GlobalBoard");
    }
    void handleChannels() override {
        home->handleChannels();
        if (home->completionValid()) {
            const auto& result = home->completion();
            board->data({0, 15}, 0, 0, 0, true, result.data.data());
            board->data({0, 15}, 1, 0, 0, true, result.data.data() + 32);
            home->popCompletion();
            done = true;
        }
    }
    void fuzz() override { fuzzer.tick(); }
    void updateSignals() override { home->updateSignals(); }
    void finalize() override {
        require(done && home->idle() && board->idle(), "borrowed Home/board resources did not drain");
        // Destroy borrowers before Sequencer reclaims memory and scoreboards.
        board.reset();
        home.reset();
        finalized = true;
    }
};

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("supply an ignored INI path");
        auto config = LoadTestConfig([](TLLocalConfig& c) {
            c.seed = 17;
            c.coreCount = 1;
            c.masterCountPerCoreTLC = 1;
            c.masterCountPerCoreTLUL = 0;
            c.memoryStart = 0x1000;
            c.memoryEnd = 0x2000;
            c.memoryPortCount = 1;
            c.memoryCycleUnit = 1;
            c.memoryReadLatency = 4;
            c.memoryReadDepth = 4;
        }, argv[1]);
        Adapter adapter;
        TLSequencer sequencer;
        sequencer.InitializeProtocol(config, adapter);
        require(sequencer.IsAlive(), "original Sequencer adapter initialization failed");
        for (uint64_t cycle = 10; cycle < 40; ++cycle) {
            sequencer.Tick(cycle);
            sequencer.Tock();
            require(sequencer.IsAlive(), "original Sequencer failed a legal adapter cycle");
        }
        sequencer.Finalize();
        require(adapter.finalized && sequencer.GetState() == TLSequencer::State::NOT_INITIALIZED,
                "original Sequencer adapter did not finalize");
        HomeAdapter homeAdapter;
        TLSequencer homeSequencer;
        homeSequencer.InitializeProtocol(config, homeAdapter);
        require(homeSequencer.IsAlive(), "borrowed Home/board initialization failed");
        for (uint64_t cycle = 10; cycle < 40; ++cycle) {
            homeSequencer.Tick(cycle);
            homeSequencer.Tock();
            require(homeSequencer.IsAlive(), "borrowed Home/board cycle failed");
        }
        TLSystemFinishEvent().Fire();
        require(homeSequencer.IsFinished(), "protocol Sequencer ignored the original fuzzer finish event");
        homeSequencer.Tick(100);
        homeSequencer.Tock();
        require(homeSequencer.IsFinished(), "finished protocol resumed scheduling");
        homeSequencer.Finalize();
        require(homeAdapter.finalized, "borrowed Home/board did not finalize");
        Adapter failure;
        failure.failCycle = true;
        TLSequencer failedSequencer;
        failedSequencer.InitializeProtocol(config, failure);
        failedSequencer.Tick(10);
        failedSequencer.Tock();
        require(failedSequencer.IsFailed(), "adapter failure was not surfaced to Sequencer state");
        failedSequencer.Finalize();
        require(failure.finalized, "failed adapter lifecycle was not finalized");
        // Default TileLink initialization and idle scheduling still use the
        // original agents/fuzzers rather than the optional protocol adapter.
        config.sequenceModes[0] = TLSequenceMode::PASSIVE;
        config.mmioEnable = false;
        TLSequencer tilelink;
        tilelink.Initialize(config);
        require(tilelink.IsAlive(), "default TileLink initialization regressed");
        for (uint64_t cycle = 1; cycle < 4; ++cycle) {
            tilelink.Tick(cycle);
            tilelink.Tock();
            require(tilelink.IsAlive(), "default TileLink idle scheduling regressed");
        }
        tilelink.Finalize();
        require(tilelink.GetState() == TLSequencer::State::NOT_INITIALIZED,
                "default TileLink finalization regressed");
        std::puts("Sequencer: original cycle/config/boards/memory, adapter failure and default TL lifecycle passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
