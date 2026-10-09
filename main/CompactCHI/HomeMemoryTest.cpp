#include "../Memory/CHIHomeMemory.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void run(unsigned seed, bool ooo) {
    TLLocalConfig cfg{};
    cfg.seed = seed;
    cfg.memoryStart = 0x1000;
    cfg.memoryEnd = 0x2000;
    cfg.memoryCycleUnit = 2;
    cfg.memoryReadLatency = 4;
    cfg.memoryWriteLatency = 3;
    cfg.memoryReadDepth = 2;
    cfg.memoryWriteDepth = 1;
    cfg.memoryOOOR = ooo;
    uint64_t cycles = 0;
    chi_agent::HomeMemory home(cfg, cycles);
    for (unsigned i = 0; i < 128; ++i) home.backend().access(0x1000 + i) = uint8_t(i ^ seed);
    home.submit({10, 0x1000, false});
    home.submit({11, 0x1040, false});
    require(!home.ready(false), "read capacity was not reserved at submission");
    bool duplicateRejected = false;
    try { home.submit({10, 0x1000, false}); }
    catch (const std::runtime_error&) { duplicateRejected = true; }
    require(duplicateRejected, "over-capacity read was accepted");
    while (!home.completionValid() && cycles < 100) { home.tick(); ++cycles; }
    require(home.completionValid() && cycles >= 8, "configured read latency was ignored");
    auto held = home.completion();
    for (unsigned i = 0; i < 8; ++i) { home.tick(); ++cycles; }
    require(!home.ready(false), "held completion lost its capacity reservation");
    require(home.completion().id == held.id && home.completion().data == held.data,
            "completion changed while the CHI consumer stalled");
    unsigned completedMask = 0;
    while (completedMask != 3 && cycles < 150) {
        if (home.completionValid()) {
            const auto& result = home.completion();
            require(result.id == 10 || result.id == 11, "unknown read ID");
            const unsigned index = result.id - 10;
            require(!(completedMask & (1U << index)), "duplicate read completion");
            for (unsigned i = 0; i < 64; ++i)
                require(result.data[i] == uint8_t((index * 64 + i) ^ seed), "read data mismatch");
            completedMask |= 1U << index;
            home.popCompletion();
        }
        home.tick(); ++cycles;
    }
    require(completedMask == 3 && home.idle(), "read resources did not drain");
    chi_agent::MemoryRequest write{20, 0x1000, true};
    write.byteEnable = 0x5555555555555555ULL;
    for (unsigned i = 0; i < 64; ++i) write.data[i] = uint8_t(i + 17);
    home.submit(write);
    require(!home.ready(true), "write capacity was not reserved");
    while (!home.completionValid() && cycles < 200) { home.tick(); ++cycles; }
    require(home.completionValid() && home.completion().write && home.completion().id == 20,
            "write response missing");
    home.popCompletion();
    home.submit({21, 0x1000, false});
    while (!home.completionValid() && cycles < 250) { home.tick(); ++cycles; }
    require(home.completionValid(), "read after write did not complete");
    for (unsigned i = 0; i < 64; ++i)
        require(home.completion().data[i] == (i % 2 == 0 ? uint8_t(i + 17) : uint8_t(i ^ seed)),
                "masked write/readback mismatch");
    home.popCompletion();
    require(home.idle(), "terminal memory resource leak");
}
int main() {
    try {
        for (unsigned seed : {42U, 7U, 12345U, 17U, 9999U}) {
            run(seed, false);
            run(seed, true);
        }
        std::puts("CHI HomeMemory: 10 seeded ordered/OOO latency, capacity, stalls and masked readback cases passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
