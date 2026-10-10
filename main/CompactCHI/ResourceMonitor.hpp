#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace compactchi {
/** Direct DUT observation; it never derives expected protocol data/permissions.
 * Counts are occupied ownership layers, not a sum of distinct transactions. */
struct ResourceSnapshot {
    std::array<unsigned, 10> counts{};
    bool ready = false;
    bool empty() const {
        if (!ready) return false;
        for (const auto count : counts) if (count) return false;
        return true;
    }
};
class ResourceMonitor {
    std::ofstream output;
    bool sawBusy = false;
    ResourceSnapshot last;
    uint64_t lastCycle = 0;
    bool sampled = false;
public:
    explicit ResourceMonitor(const std::string& path) : output(path) {
        if (!output) throw std::runtime_error("cannot open internal resource observations");
    }
    void sample(uint64_t cycle, uint64_t time, const ResourceSnapshot& snapshot) {
        if (sampled && cycle != lastCycle + 1) throw std::runtime_error("resource sampling is not continuous");
        static constexpr const char* names[] = {"mshrs", "responses", "writebacks", "requests", "queues",
            "pipeline", "serializers", "controllers", "retryOwners", "credits"};
        output << "{\"schema\":1,\"cycle\":" << cycle << ",\"sim_time\":" << time
            << ",\"ready\":" << (snapshot.ready ? "true" : "false");
        for (unsigned i = 0; i < snapshot.counts.size(); ++i) {
            output << ",\"" << names[i] << "\":" << snapshot.counts[i];
            if (snapshot.counts[i]) sawBusy = true;
        }
        output << "}\n"; output.flush();
        last = snapshot; lastCycle = cycle; sampled = true;
    }
    void finish() const {
        if (!sampled || !sawBusy || !last.empty())
            throw std::runtime_error("DUT resource observation did not cover activity and terminal drain");
    }
};
} // namespace compactchi
