#include "../Memory/MemoryAgent.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void exercise(bool outOfOrder) {
    TLLocalConfig config{};
    config.seed = 42;
    config.memoryStart = 0x1000;
    config.memoryEnd = 0x2000;
    config.memoryReadDepth = 8;
    config.memoryWriteDepth = 4;
    config.memoryReadLatency = 3;
    config.memoryWriteLatency = 2;
    config.memoryCycleUnit = 2;
    config.memoryOOOR = outOfOrder;
    uint64_t cycles = 0;
    axi_agent::MemoryAgent memory(&config, 42, &cycles, 0);
    axi_agent::Bundle<BEATSIZE_MEMORY> port{};
    memory.connect(&port);
    memory.update_signal();
    auto tick = [&]() {
        memory.handle_channel();
        ++cycles;
        memory.update_signal();
    };

    // Two independent reads go through the original latency/OOO queues.
    for (unsigned i = 0; i < 128; ++i) memory.access(0x1000 + i) = uint8_t(i ^ 0xa5);
    for (unsigned id = 1; id <= 2; ++id) {
        port.ar.id = id;
        port.ar.addr = 0x1000 + (id - 1) * 64;
        port.ar.len = 1;
        port.ar.size = 5;
        port.ar.burst = axi_agent::BURST_INCR;
        port.ar.valid = 1;
        require(port.ar.ready, "initial MemoryAgent AR blocked");
        tick();
    }
    port.ar.valid = 0;
    while (cycles < 6) {
        require(!port.r.valid, "MemoryAgent returned data before configured latency");
        tick();
    }
    unsigned masks[3] = {};
    unsigned counts[3] = {};
    unsigned total = 0;
    bool stalled = false;
    while (cycles < 100 && total != 4) {
        if (port.r.valid && !stalled) {
            const auto data = *port.r.data;
            const unsigned id = port.r.id;
            const unsigned last = port.r.last;
            port.r.ready = 0;
            for (unsigned i = 0; i < 5; ++i) {
                tick();
                require(port.r.valid && port.r.id == id && port.r.last == last,
                        "MemoryAgent R identity changed during backpressure");
                for (unsigned b = 0; b < 32; ++b)
                    require(port.r.data->data[b] == data.data[b], "MemoryAgent R data changed during stall");
            }
            stalled = true;
        }
        port.r.ready = 1;
        if (port.r.fire()) {
            const unsigned id = port.r.id;
            require(id == 1 || id == 2, "unknown MemoryAgent RID");
            const unsigned beat = counts[id]++;
            require(beat < 2 && port.r.last == (beat == 1), "MemoryAgent read beat framing");
            for (unsigned b = 0; b < 32; ++b)
                require(port.r.data->data[b] == uint8_t(((id - 1) * 64 + beat * 32 + b) ^ 0xa5),
                        "MemoryAgent read data mismatch");
            masks[id] |= 1U << beat;
            ++total;
        }
        tick();
    }
    require(total == 4 && masks[1] == 3 && masks[2] == 3, "MemoryAgent read did not drain");
    port.r.ready = 0;

    port.aw.valid = 1;
    port.aw.id = 7;
    port.aw.addr = 0x1080;
    port.aw.size = 5;
    port.aw.len = 1;
    port.aw.burst = axi_agent::BURST_INCR;
    tick();
    port.aw.valid = 0;
    for (unsigned beat = 0; beat < 2; ++beat) {
        port.w.valid = 1;
        port.w.last = beat == 1;
        port.w.strb = 0x55555555;
        port.w.data = make_shared_tldata<32>();
        for (unsigned b = 0; b < 32; ++b) port.w.data->data[b] = uint8_t(beat * 32 + b);
        require(port.w.ready, "MemoryAgent W stalled unexpectedly");
        tick();
    }
    port.w.valid = 0;
    for (unsigned i = 0; i < 64; ++i)
        require(memory.access(0x1080 + i) == (i % 2 == 0 ? uint8_t(i) : 0),
                "MemoryAgent byte enable write mismatch");
    while (!port.b.valid && cycles < 150) tick();
    require(port.b.valid && port.b.id == 7, "MemoryAgent write response missing");
    for (unsigned i = 0; i < 4; ++i) {
        tick();
        require(port.b.valid && port.b.id == 7, "MemoryAgent B changed during stall");
    }
    port.b.ready = 1;
    tick();
    require(!port.b.valid, "MemoryAgent write response did not retire");
    cycles = (uint64_t(1) << 40) + 17;
    require(memory.cycle() == cycles, "MemoryAgent cycle truncation");
}
}

int main() {
    try {
        exercise(false);
        exercise(true);
        std::puts("MemoryAgent: latency, OOO, masked writes, stalled responses and 64-bit cycle passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
