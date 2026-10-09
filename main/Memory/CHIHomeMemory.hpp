#pragma once

#include "MemoryAgent.hpp"
#include <array>
#include <deque>
#include <map>
#include <memory>

namespace chi_agent {

using Line = std::array<uint8_t, DATASIZE>;
struct MemoryRequest {
    uint32_t id;
    uint64_t address;
    bool write;
    Line data{};
    uint64_t byteEnable = ~uint64_t(0);
};
struct MemoryCompletion {
    uint32_t id;
    uint64_t address;
    bool write;
    Line data{};
};

/** CHI Home's memory backend adapter. AXI here is private test software:
 * the AnserL2 downstream ports remain CHI E.b. All data, latency, queue
 * capacity and read reordering come from the original MemoryAgent. */
class HomeMemory {
    struct PendingRead { uint64_t address; unsigned beats = 0; Line data{}; };
    struct PendingWrite { MemoryRequest request; unsigned beat = 0; bool addressSent = false; };
    TLLocalConfig& config;
    uint64_t& cycles;
    axi_agent::Bundle<BEATSIZE_MEMORY> ownedPort{};
    axi_agent::Bundle<BEATSIZE_MEMORY>& port;
    std::unique_ptr<axi_agent::MemoryAgent> ownedMemory;
    axi_agent::MemoryAgent& memory;
    std::deque<MemoryRequest> readQueue;
    std::deque<PendingWrite> writeQueue;
    std::map<uint32_t, PendingRead> reads;
    std::map<uint32_t, uint64_t> writes;
    std::deque<MemoryCompletion> completed;
    void drive();
public:
    HomeMemory(TLLocalConfig& config, uint64_t& cycles);
    HomeMemory(TLLocalConfig& config, uint64_t& cycles,
               axi_agent::MemoryAgent& memory, axi_agent::Bundle<BEATSIZE_MEMORY>& port);
    // Borrowed mode is stepped by the original Sequencer around these hooks.
    void handleChannels();
    void updateSignals();
    bool ready(bool write) const;
    void submit(const MemoryRequest& request);
    void tick();
    bool completionValid() const { return !completed.empty(); }
    const MemoryCompletion& completion() const { return completed.front(); }
    void popCompletion();
    bool idle() const;
    axi_agent::MemoryAgent& backend() { return memory; }
};

} // namespace chi_agent
