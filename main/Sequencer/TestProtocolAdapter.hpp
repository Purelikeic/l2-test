#pragma once

#include "../Base/TLLocal.hpp"
#include "../Memory/MemoryAgent.hpp"
#include "../Utils/ScoreBoard.h"
#include "../Utils/ULScoreBoard.h"

// Add a protocol boundary to the original Sequencer, rather than another
// simulator loop. Adapter lifetime is owned by the caller through Finalize.
class TestProtocolAdapter {
public:
    virtual ~TestProtocolAdapter() = default;
    virtual void initialize(TLLocalConfig& config, uint64_t& cycles,
                            GlobalBoard<paddr_t>& global, UncachedBoard<paddr_t>& uncached,
                            axi_agent::MemoryAgent& memory,
                            axi_agent::Bundle<BEATSIZE_MEMORY>& memoryPort) = 0;
    virtual void handleChannels() = 0;
    virtual void fuzz() = 0;
    virtual void updateSignals() = 0;
    virtual void finalize() = 0;
};
