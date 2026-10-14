#pragma once

#include "EbHome.hpp"
#include <stdexcept>

namespace compactchi {
/** Bounded reference-vector adapter, not a full L2 scoreboard.
 * Default AnserL2 geometry: four slices, 512 sets, eight ways. CPL2 demand
 * refills insert at RRPV=0; the equal-way tie ages peers and selects A/B/C/D/E.
 * Root BOP auxiliary traffic is checked by EbHome but is not primary residency
 * evidence. No DUT signal is used to derive the expected victim or dirty data. */
class VictimProfile {
    uint64_t base, end;
    unsigned victims = 0, clean = 0, primaryReads = 0;
public:
    static constexpr unsigned ways = 8;
    static constexpr unsigned fills = ways + 4;
    static constexpr unsigned requests = fills + 2;
    static constexpr uint64_t memoryEnd = 0x200000;
    static constexpr uint64_t victimStride = 4 * 512 * 64;
    VictimProfile(uint64_t base = 0x1000, uint64_t end = memoryEnd) : base(base), end(end) {}
    static uint64_t address(unsigned i, uint64_t base = 0x1000) {
        return base + uint64_t(i) * victimStride;
    }
    static uint64_t requestAddress(unsigned i, uint64_t base = 0x1000) {
        return address(i < fills ? i : i == fills ? 0 : ways + 1, base);
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
        // PBOP +1 auxiliary traffic goes to the next Slice; it is not a
        // primary victim observation and still passes the EbHome data checks.
        if ((request.address & (victimStride - 1)) == ((base + 64) & (victimStride - 1))) {
            if (request.address >= end || (request.opcode != 0x26 && request.opcode != 0x42))
                throw std::runtime_error("victim profile auxiliary prefetch violated its clean traffic boundary");
            return plan;
        }
        if ((request.address & (victimStride - 1)) != (base & (victimStride - 1)) || request.address >= end)
            throw std::runtime_error("victim profile observed traffic outside its fixed vectors");
        if (!release) {
            // Thirteen planned misses; the final request to resident J must hit.
            if (primaryReads >= fills + 1 || request.address != requestAddress(primaryReads, base) ||
                request.opcode != (primaryReads == 0 ? 7U : 0x26U))
                throw std::runtime_error("victim profile acquisition differs from fixed miss/hit vectors");
            ++primaryReads;
            return plan;
        }
        if (victims >= 5 || primaryReads != victims + ways + 1 || request.address != address(victims, base))
            throw std::runtime_error("victim address differs from independent CPL2 RRIP vectors");
        const auto expectedOpcode = victims == 0 ? 0x1bU : 0x42U;
        if (request.opcode != expectedOpcode || (victims == 0 && !dirtyStored))
            throw std::runtime_error("victim writeback classification differs from planned dirty ownership");
        if (victims != 0) plan.cleanCopyback = (++clean % 2) == 0;
        ++victims;
        return plan;
    }
    bool complete() const { return dirtyStored && victims == 5 && clean == 4 && primaryReads == fills + 1; }
};
} // namespace compactchi
