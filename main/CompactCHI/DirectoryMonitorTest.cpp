#include "DirectoryMonitor.hpp"
#include <functional>
#include <cstdio>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Directory monitor output path missing");
        unsigned rejected = 0;
        auto run = [&](unsigned mutation) {
            compactchi::DirectoryMonitor monitor(argv[1], 2, 2, 4, 64, 48);
            compactchi::DirectoryClients expected{{{0, 4096}, 2}, {{2, 4672}, 1}};
            for (unsigned index = 0; index < 4; ++index) {
                std::array<compactchi::DirectoryEntry, 4> row{};
                if (!index) {
                    row[0] = {8, 2, 2, 0, true};
                    row[1] = {9, 3, 1, 2, true};
                    row[2] = {0, 0, 3, 127, false}; // Invalid stale payload is not a client.
                    row[3] = {4, 1, 0, 0, false}; // A resident with no client is legal.
                    if (mutation == 1) row[0].client = false;
                    if (mutation == 2) row[0].clientID = 3;
                    if (mutation == 3) row[1].clientState = 2;
                    if (mutation == 4) row[0].state = 0;
                    if (mutation == 5) row[0].clientState = 0;
                    if (mutation == 6) row[2].client = true;
                    if (mutation == 7) row[3] = {4, 2, 2, 3, true};
                    if (mutation == 8) row[0].tag ^= 1;
                }
                if (mutation == 9 && index == 1) row[0] = {8, 1, 0, 0, false};
                if (mutation == 10 && index == 3) break;
                const auto cycle = 30 + index + (mutation == 11 && index > 0 ? 1 : 0);
                monitor.sample(cycle, cycle * 2 + (mutation == 12), mutation != 13,
                    index / 2, mutation == 14 ? 0 : index % 2, row);
            }
            monitor.finish(expected);
        };
        run(0);
        for (unsigned mutation = 1; mutation <= 14; ++mutation) {
            bool failed = false;
            try { run(mutation); } catch (const std::runtime_error&) { failed = true; }
            if (!failed) throw std::runtime_error("Directory monitor accepted a corrupted state/scan");
            ++rejected;
        }
        run(0); // Leave a complete positive fixture for the independent checker.
        std::printf("Directory monitor: full coverage, independent client footprint and %u invalid state/scan observations rejected\n", rejected);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
