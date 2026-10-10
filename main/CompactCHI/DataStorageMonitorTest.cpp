#include "DataStorageMonitor.hpp"
#include <cstdio>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("DataStorage monitor output path missing");
        auto run = [&](unsigned mutation) {
            compactchi::DataStorageMonitor monitor(argv[1], 2, 2, 4, 64, 48);
            for (unsigned index = 0; index < 4; ++index) {
                std::array<compactchi::DataStorageLine, 4> lines{};
                std::array<bool, 4> errors{};
                for (unsigned slice = 0; slice < 4; ++slice)
                    for (unsigned byte = 0; byte < 64; ++byte) lines[slice][byte] = uint8_t(index * 17 + slice * 3 + byte);
                if (mutation == 6 && index == 3) break;
                if (mutation == 3 && index == 1) monitor.sample(30 + index, (30 + index) * 2, false, index / 2, index % 2, lines, errors);
                else if (mutation == 4 && index == 2) monitor.sample(30 + index + 1, (30 + index + 1) * 2, true, index / 2, index % 2, lines, errors);
                else if (mutation == 5 && index == 1) monitor.sample(30 + index, (30 + index) * 2, true, 1, index % 2, lines, errors);
                else monitor.sample(30 + index, (30 + index) * 2, true, index / 2, index % 2, lines, errors);
            }
            monitor.finish();
        };
        run(0);
        unsigned rejected = 0;
        for (unsigned mutation = 3; mutation <= 6; ++mutation) {
            bool failed = false;
            try { run(mutation); } catch (const std::runtime_error&) { failed = true; }
            if (!failed) throw std::runtime_error("DataStorage monitor accepted a scan mutation");
            ++rejected;
        }
        run(0);
        std::printf("DataStorage monitor: complete indexed scan and %u drain/coordinate mutations rejected\n", rejected);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
