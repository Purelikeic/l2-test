#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace compactchi {
using DataStorageLine = std::array<uint8_t, 64>;
class DataStorageMonitor {
    std::ofstream output;
    unsigned sets, ways, slices, lineBytes, addrBits, next = 0;
    uint64_t lastCycle = 0;
    static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
public:
    DataStorageMonitor(const std::string& path, unsigned sets, unsigned ways, unsigned slices,
                       unsigned lineBytes, unsigned addrBits)
        : output(path), sets(sets), ways(ways), slices(slices), lineBytes(lineBytes), addrBits(addrBits) {
        require(bool(output) && sets && ways && slices == 4 && lineBytes == 64 && addrBits > 8 && addrBits < 64,
                "invalid DataStorage observation geometry/output");
        output << "{\"schema\":1,\"sets\":" << sets << ",\"ways\":" << ways << ",\"slices\":" << slices
               << ",\"lineBytes\":" << lineBytes << ",\"addrBits\":" << addrBits << "}\n";
    }
    unsigned index() const { return next; }
    unsigned set() const { return next / ways; }
    unsigned way() const { return next % ways; }
    bool complete() const { return next == sets * ways; }
    void sample(uint64_t cycle, uint64_t time, bool drained, unsigned set, unsigned way,
                const std::array<DataStorageLine, 4>& lines, const std::array<bool, 4>& errors) {
        require(!complete() && drained && time == cycle * 2 && set == this->set() && way == this->way() &&
                (!next || cycle == lastCycle + 1), "DataStorage scan escaped drain or skipped an index");
        output << "{\"cycle\":" << cycle << ",\"sim_time\":" << time << ",\"set\":" << set
               << ",\"way\":" << way << ",\"lines\":[";
        for (unsigned slice = 0; slice < slices; ++slice) {
            if (slice) output << ',';
            output << "{\"error\":" << (errors[slice] ? "true" : "false") << ",\"data\":\"";
            static constexpr char digits[] = "0123456789abcdef";
            for (const auto byte : lines[slice]) output << digits[byte >> 4] << digits[byte & 15];
            output << "\"}";
        }
        output << "]}\n"; output.flush();
        require(bool(output), "cannot write DataStorage observation");
        lastCycle = cycle; ++next;
    }
    void finish() const { require(complete(), "DataStorage terminal scan is incomplete"); }
};
} // namespace compactchi
