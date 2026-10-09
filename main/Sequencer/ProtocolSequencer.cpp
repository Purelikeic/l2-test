#include "TLSequencer.hpp"
#include "../Events/TLSystemEvent.hpp"
#include <algorithm>
#include <stdexcept>

void TLSequencer::InitializeProtocol(const TLLocalConfig& cfg, TestProtocolAdapter& adapter) noexcept {
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
        if (config.memoryPortCount != 1 || config.memoryEnd <= config.memoryStart || !GetAgentCount())
            throw std::invalid_argument("protocol adapter requires one memory port, a memory region and node identities");
        globalBoard = new GlobalBoard<paddr_t>;
        uncachedBoard = new UncachedBoard<paddr_t>(GetAgentCount());
        pmem = new uint8_t[config.memoryEnd - config.memoryStart]();
        memoryAXI = new MemoryAXIPort*[1]{};
        memories = new MemoryAgent*[1]{};
        memoryAXI[0] = new MemoryAXIPort{};
        memoryAXI[0]->w.data = make_shared_tldata_zero<BEATSIZE_MEMORY>();
        memoryAXI[0]->r.data = make_shared_tldata_zero<BEATSIZE_MEMORY>();
        memories[0] = new MemoryAgent(&config, config.seed, &cycles, 0, pmem);
        memories[0]->connect(memoryAXI[0]);
        adapter.initialize(config, cycles, *globalBoard, *uncachedBoard, *memories[0], *memoryAXI[0]);
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
        protocol->fuzz();
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
