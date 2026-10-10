#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace compactchi {
using DirectoryClients = std::map<std::pair<unsigned, uint64_t>, unsigned>;
struct DirectoryEntry {
    uint64_t tag;
    unsigned state, clientState, clientID;
    bool client;
};
/** Current Directory storage only. Expectations come from the independent node
 * model, never from observed tag/meta writes or replacement choices. */
class DirectoryMonitor {
    std::ofstream output;
    unsigned sets, ways, slices, lineBytes, addrBits, next = 0;
    uint64_t lastCycle = 0;
    std::set<uint64_t> resident;
    DirectoryClients clients;
    static void require(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }
    static bool powerOfTwo(unsigned value) { return value && !(value & (value - 1)); }
public:
    DirectoryMonitor(const std::string& path, unsigned sets, unsigned ways, unsigned slices,
                     unsigned lineBytes, unsigned addrBits)
        : output(path), sets(sets), ways(ways), slices(slices), lineBytes(lineBytes), addrBits(addrBits) {
        require(bool(output) && powerOfTwo(sets) && powerOfTwo(ways) && slices == 4 && lineBytes == 64 &&
                addrBits > 8 && addrBits < 64 && uint64_t(sets) * slices * lineBytes < (uint64_t(1) << addrBits),
                "invalid Directory observation geometry/output");
        output << "{\"schema\":1,\"sets\":" << sets << ",\"ways\":" << ways << ",\"slices\":" << slices
               << ",\"lineBytes\":" << lineBytes << ",\"addrBits\":" << addrBits << "}\n";
    }
    unsigned index() const { return next; }
    unsigned set() const { return next / ways; }
    unsigned way() const { return next % ways; }
    bool complete() const { return next == sets * ways; }
    void sample(uint64_t cycle, uint64_t time, bool drained, unsigned set, unsigned way,
                const std::array<DirectoryEntry, 4>& entries) {
        require(!complete() && drained, "Directory scan escaped drained state or exceeded coverage");
        require(set == this->set() && way == this->way() && time == cycle * 2 &&
                (!next || cycle == lastCycle + 1), "Directory scan skipped/repeated an index or cycle");
        output << "{\"cycle\":" << cycle << ",\"sim_time\":" << time << ",\"set\":" << set
               << ",\"way\":" << way << ",\"entries\":[";
        for (unsigned slice = 0; slice < slices; ++slice) {
            const auto& e = entries[slice];
            require(e.state <= 3 && (!e.client || (e.state != 0 && (e.clientState == 1 || e.clientState == 2))),
                    "Directory client resides in Invalid or reserved permission state");
            const uint64_t stride = uint64_t(sets) * slices * lineBytes;
            require(e.tag < (uint64_t(1) << addrBits) / stride, "Directory tag exceeds address width");
            const uint64_t address = ((e.tag * sets + set) * slices + slice) * lineBytes;
            if (e.state) require(resident.insert(address).second, "Directory duplicated a valid resident tag");
            if (e.client) require(clients.emplace(std::make_pair(e.clientID, address), e.clientState).second,
                                  "Directory duplicated a persistent client");
            if (slice) output << ',';
            output << "{\"tag\":" << e.tag << ",\"state\":" << e.state << ",\"client\":" << (e.client ? "true" : "false")
                   << ",\"clientState\":" << e.clientState << ",\"clientID\":" << e.clientID << '}';
        }
        output << "]}\n"; output.flush();
        require(bool(output), "cannot write Directory observation");
        lastCycle = cycle; ++next;
    }
    void finish(const DirectoryClients& expected) const {
        require(complete(), "Directory terminal scan is incomplete");
        require(clients == expected, "Directory terminal client footprint differs from independent requester state");
    }
};
} // namespace compactchi
