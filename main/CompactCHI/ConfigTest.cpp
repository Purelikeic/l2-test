#include "../System/TLConfig.hpp"
#include "../Utils/Common.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("supply an ignored build directory");
        auto path = std::filesystem::path(argv[1]) / "config-test.ini";
        std::ofstream ini(path);
        ini << "[tltest.config]\nmaximum_cycle=777\nmemory.backend.ooor=0\n"
            << "memory.backend.rd.latency=9\nmemory.backend.cycle_unit=3\n"
            << "[tltest.fuzzer]\nseed=12345\nari.interval=123\nari.target=4\n"
            << "[tltest.sequence]\nmode.0=fuzz_stream\n";
        ini.close();
        const auto config = LoadTestConfig([](TLLocalConfig& config) {
            config.seed = 42;
            config.memoryReadLatency = 5;
        }, path.string());
        if (config.seed != 12345 || config.maximumCycle != 777 || config.memoryOOOR != 0 ||
            config.memoryReadLatency != 9 || config.memoryCycleUnit != 3 ||
            config.fuzzARIInterval != 123 || config.fuzzARITarget != 4 ||
            config.sequenceModes.at(0) != TLSequenceMode::FUZZ_STREAM)
            throw std::runtime_error("shared System config override mismatch");
        const auto defaults = LoadTestConfig([](TLLocalConfig& c) { c.seed = 17; },
            (path.parent_path() / "absent-config.ini").string());
        if (defaults.seed != 17 || defaults.memoryReadLatency != TLTEST_DEFAULT_MEMORY_READ_LATENCY)
            throw std::runtime_error("shared System default/callback order mismatch");
        std::puts("System config: original defaults, callback, INI, seed and fuzzer modes passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
