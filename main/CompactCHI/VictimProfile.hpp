#pragma once

#include "EbHome.hpp"
#include <stdexcept>

namespace compactchi {
/** Bounded reference-vector adapter, not a full L2 scoreboard.
 * Default AnserL2 geometry: four slices, 128 sets, four ways. CPL2 demand
 * refills insert at RRPV=0; the equal-way tie ages peers and selects A/B/C/D/E.
 * Root BOP auxiliary traffic is checked by EbHome but is not primary residency
 * evidence. No DUT signal is used to derive the expected victim or dirty data. */
class VictimProfile {
    uint64_t base, end;
    unsigned victims = 0, clean = 0, primaryReads = 0;
public:
    static constexpr unsigned requests = 10;
    static constexpr uint64_t memoryEnd = 0x40000;
    VictimProfile(uint64_t base = 0x1000, uint64_t end = memoryEnd) : base(base), end(end) {}
    static uint64_t address(unsigned i, uint64_t base = 0x1000) {
        return base + uint64_t(i) * 0x8000;
    }
    static uint64_t requestAddress(unsigned i, uint64_t base = 0x1000) {
        return address(i < 8 ? i : i == 8 ? 0 : 5, base);
    }
    static chi_agent::Line initial(uint64_t address, unsigned seed) {
        chi_agent::Line bytes{};
        for (unsigned i = 0; i < 64; ++i) bytes[i] = uint8_t((address >> 6) ^ (i * 17) ^ seed);
        return bytes;
    }
    static chi_agent::Line dirty(unsigned seed, uint64_t base = 0x1000) {
        auto bytes = initial(base, seed);
        bytes[0] ^= 0xa5;
        bytes[63] ^= 0x5a;
        return bytes;
    }
    bool dirtyStored = false;
    HomePlan observe(const EbRequest& request) {
        HomePlan plan;
        plan.responseTraceTag = true;
        const bool release = request.opcode == 0x42 || request.opcode == 0x1b;
        // The unchanged root BOP uses offset +1 for this stride and occupies
        // the next slice. EbHome still checks its data/transaction obligations.
        if ((request.address & 0x7fff) == ((base + 64) & 0x7fff)) {
            if (request.address >= end || (request.opcode != 0x26 && request.opcode != 0x42))
                throw std::runtime_error("victim profile auxiliary prefetch violated its clean traffic boundary");
            return plan;
        }
        if ((request.address & 0x7fff) != (base & 0x7fff) || request.address >= end)
            throw std::runtime_error("victim profile observed traffic outside its fixed vectors");
        if (!release) {
            // Nine planned misses; the final request to resident F must hit.
            if (primaryReads >= 9 || request.address != requestAddress(primaryReads, base) ||
                request.opcode != (primaryReads == 0 ? 7U : 0x26U))
                throw std::runtime_error("victim profile acquisition differs from fixed miss/hit vectors");
            ++primaryReads;
            return plan;
        }
        if (victims >= 5 || primaryReads != victims + 5 || request.address != address(victims, base))
            throw std::runtime_error("victim address differs from independent CPL2 RRIP vectors");
        const auto expectedOpcode = victims == 0 ? 0x1bU : 0x42U;
        if (request.opcode != expectedOpcode || (victims == 0 && !dirtyStored))
            throw std::runtime_error("victim writeback classification differs from planned dirty ownership");
        if (victims != 0) plan.cleanCopyback = (++clean % 2) == 0;
        ++victims;
        return plan;
    }
    bool complete() const { return dirtyStored && victims == 5 && clean == 4 && primaryReads == 9; }
};
} // namespace compactchi
