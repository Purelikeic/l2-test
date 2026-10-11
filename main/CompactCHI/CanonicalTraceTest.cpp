#include "CanonicalTrace.hpp"
#include <cstdio>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("supply ignored trace prefix");
        compactchi::CanonicalTrace trace(argv[1], 42);
        compactchi::CanonicalEvent event{42, 85, compactchi::Channel::CchiTxDat,
            1, 2, 7, 0, true, false, true};
        event.address = 0x1000;
        event.dbid = 9;
        event.dataId = 0;
        event.resp = 2;
        event.respErr = 0;
        event.beforeState = 0;
        event.afterState = 2;
        event.owner = 2;
        event.dataSource = 3;
        event.prefetchSource = 0;
        event.dirty = false;
        event.allowRetry = false; event.expCompAck = true; event.pCrdType = 3;
        event.size = 6; event.memAttr = 0xd; event.order = 0;
        event.ns = false; event.likelyShared = true;
        event.data = std::array<uint8_t, 32>{};
        for (unsigned i = 0; i < 32; ++i) (*event.data)[i] = uint8_t(i ^ 0xa5);
        trace.record(event);
        event.cycle = 43;
        event.simTime = 87;
        event.ready = true;
        trace.record(event);
        for (unsigned i = 0; i < 13; ++i)
            trace.record({45 + i, std::nullopt, compactchi::Channel(i), 2, 1, 7, i,
                          true, true, (i & 1) != 0});
        bool rejected = false;
        event.cycle = 1;
        try { trace.record(event); } catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("trace accepted a backwards cycle");
        std::puts("CanonicalTrace: all channel directions, handshake stalls and monotonic cycles passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
