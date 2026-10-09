#include "TLSequencer.hpp"
#include "../Events/TLSystemEvent.hpp"
#include <algorithm>
#include <stdexcept>

void TLSequencer::InitializeProtocol(const TLLocalConfig& cfg, TestProtocolAdapter& adapter) noexcept {
    Initialize(cfg, adapter); // Compatibility for memory-only host fixtures.
}

void TLSequencer::Initialize(const TLLocalConfig& cfg, TestProtocolAdapter& adapter) noexcept {
    if (state != State::NOT_INITIALIZED || protocol) return;
    protocol = &adapter;
    try {
        config = cfg;
        Gravity::RegisterListener(Gravity::MakeListener<TLSystemFinishEvent>(
            Gravity::StringAppender("tltest.protocol.finish", uint64_t(this)).ToString(), 0,
            [this](TLSystemFinishEvent&) {
                if (IsAlive()) state = State::FINISHED;
            }));
        TLSystemInitializationPreEvent(&config).Fire();
        if (config.memoryPortCount != 1 || config.memoryEnd <= config.memoryStart || TestNodeLayout(config).empty())
            throw std::invalid_argument("protocol adapter requires one memory port, a memory region and node identities");
        globalBoard = new GlobalBoard<paddr_t>;
        const auto layout = TestNodeLayout(config);
        uncachedBoard = new UncachedBoard<paddr_t>(adapter.hasActionNodes() ? layout.size() : GetAgentCount());
        pmem = new uint8_t[config.memoryEnd - config.memoryStart]();
        memoryAXI = new MemoryAXIPort*[1]{};
        memories = new MemoryAgent*[1]{};
        memoryAXI[0] = new MemoryAXIPort{};
        memoryAXI[0]->w.data = make_shared_tldata_zero<BEATSIZE_MEMORY>();
        memoryAXI[0]->r.data = make_shared_tldata_zero<BEATSIZE_MEMORY>();
        memories[0] = new MemoryAgent(&config, config.seed, &cycles, 0, pmem);
        memories[0]->connect(memoryAXI[0]);
        adapter.initialize(config, cycles, *globalBoard, *uncachedBoard, *memories[0], *memoryAXI[0]);
        if (adapter.hasActionNodes()) {
            std::vector<TestNode*> views;
            for (const auto& descriptor : layout) {
                auto node = adapter.createNode(descriptor);
                if (!node || node->descriptor.id != descriptor.id || node->descriptor.kind != descriptor.kind)
                    throw std::invalid_argument("protocol factory failed to create the configured node");
                views.push_back(node.get());
                actionNodes.push_back(std::move(node));
            }
            actionFuzzer = adapter.createFuzzer(config, views);
            if (!actionFuzzer) throw std::invalid_argument("action factory has no common node fuzzer");
            actionFuzzer->set_cycles(&cycles);
        }
        state = State::ALIVE;
        TLSystemInitializationPostEvent(this).Fire();
    } catch (const std::exception& e) {
        state = State::FAILED;
        LogError(cycles, Append("Protocol initialization failed: ", e.what()).EndLine());
    }
}

void TLSequencer::TockProtocol() noexcept {
    if (!IsAlive()) return;
    try {
        // Capture both protocol and memory handshakes before the adapter can
        // replace an offered memory request for the next cycle.
        protocol->handleChannels();
        memories[0]->handle_channel();
        if (actionFuzzer) actionFuzzer->tick();
        protocol->fuzz(); // Environment service/finish only for action-node adapters.
        // MemoryAgent::send_r uses the sampled ready to hold/advance a
        // response. Generate memory outputs before driving next-cycle ready.
        memories[0]->update_signal();
        protocol->updateSignals();
    } catch (const std::exception& e) {
        state = State::FAILED;
        LogError(cycles, Append("Protocol cycle failed: ", e.what()).EndLine());
    }
}

void TLSequencer::FinalizeProtocol() noexcept {
    try {
        TLSystemFinalizationPreEvent(this).Fire();
        actionFuzzer.reset();
        actionNodes.clear();
        protocol->finalize();
    } catch (const std::exception& e) {
        state = State::FAILED;
        LogError(cycles, Append("Protocol finalization failed: ", e.what()).EndLine());
    }
    Gravity::UnregisterListener<TLSystemFinishEvent>(
        Gravity::StringAppender("tltest.protocol.finish", uint64_t(this)).ToString());
    if (memories) { delete memories[0]; delete[] memories; memories = nullptr; }
    if (memoryAXI) { delete memoryAXI[0]; delete[] memoryAXI; memoryAXI = nullptr; }
    delete[] pmem; pmem = nullptr;
    delete globalBoard; globalBoard = nullptr;
    delete uncachedBoard; uncachedBoard = nullptr;
    protocol = nullptr;
    // Preserve failure for the simulator's exit decision; successful lifecycle
    // returns to the existing NOT_INITIALIZED state.
    if (state != State::FAILED) state = State::NOT_INITIALIZED;
    TLSystemFinalizationPostEvent().Fire();
}
