#pragma once
#include "EbHome.hpp"
#include <map>
#include <optional>
#include <vector>

namespace compactchi {
/** Independent external forward endpoint. It owns DCT data collection and
 * emits a later CompAck obligation; no DUT data becomes expected state. */
class ForwardNodeBoard {
public:
    struct Observation {
        const char* kind;
        uint64_t cycle;
        unsigned txn, dbid, source, target, beat;
        bool traceTag;
    };
private:
    struct Entry {
        unsigned node, target, dbid, txn;
        bool traceTag;
        unsigned mask = 0;
        uint64_t readyCycle = 0;
    };
    std::map<std::pair<unsigned, unsigned>, Entry> active;
    std::vector<Observation> log;
public:
    void accept(const EbData& data, uint64_t cycle) {
        if (data.target != 12 || data.opcode != 4 || data.home != 8 || data.dataId > 2 || data.respErr != 0)
            throw std::runtime_error("forward endpoint received an invalid DCT data flit");
        const auto key = std::make_pair(data.target, data.txn);
        auto [it, inserted] = active.emplace(key, Entry{data.target, data.home, data.dbid, data.txn, data.traceTag});
        auto& entry = it->second;
        if (!inserted && (entry.dbid != data.dbid || entry.traceTag != data.traceTag))
            throw std::runtime_error("forward endpoint DCT identity changed");
        const unsigned beat = data.dataId / 2;
        if (entry.mask & (1U << beat)) throw std::runtime_error("forward endpoint received duplicate DCT beat");
        entry.mask |= 1U << beat;
        log.push_back({"data", cycle, data.txn, data.dbid, data.source, data.target, beat, data.traceTag});
        if (entry.mask == 3) entry.readyCycle = cycle + 1;
    }
    std::optional<EbResponse> tick(uint64_t cycle) {
        for (auto it = active.begin(); it != active.end(); ++it) {
            auto& entry = it->second;
            if (entry.mask == 3 && cycle >= entry.readyCycle) {
                EbResponse ack{12, entry.target, entry.dbid, 2, 0, 0, entry.traceTag};
                log.push_back({"ack", cycle, entry.dbid, entry.dbid, 12, entry.target, 3, entry.traceTag});
                active.erase(it);
                return ack;
            }
        }
        return std::nullopt;
    }
    const std::vector<Observation>& observations() const { return log; }
    bool idle() const { return active.empty(); }
};
} // namespace compactchi
