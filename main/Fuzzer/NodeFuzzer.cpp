#include "NodeFuzzer.hpp"
#include <algorithm>

NodeFuzzer::NodeFuzzer(const TLLocalConfig& cfg, const std::vector<TestNode*>& nodes,
                     TestScenarioHooks& hooks, TestScenario requested, std::vector<PlannedTestAction> replay)
    : cfg(cfg), nodes(nodes), hooks(hooks), scenario(requested), random(cfg.seed ^ 0x9e3779b9U), replay(std::move(replay)),
      streamNext(nodes.size(), cfg.fuzzStreamStart), streamAt(nodes.size(), cfg.fuzzStreamInterval) {
    startupInterval = 0;
    std::optional<TestScenario> declared;
    for (const auto& [id, mode] : cfg.sequenceModes) {
        std::optional<TestScenario> selected;
        switch (mode) {
        case TLSequenceMode::DETERMINISTIC_READ: selected = TestScenario::DeterministicRead; break;
        case TLSequenceMode::RANDOM_READ: selected = TestScenario::RandomRead; break;
        case TLSequenceMode::LOWER_SNOOP: selected = TestScenario::LowerSnoop; break;
        case TLSequenceMode::VICTIM: selected = TestScenario::Victim; break;
        case TLSequenceMode::NODE_ACTIONS: selected = TestScenario::Actions; break;
        case TLSequenceMode::RANDOM_ACTIONS: selected = TestScenario::RandomActions; break;
        default: break;
        }
        if (selected) {
            if (id < 0 || unsigned(id) >= nodes.size() || (declared && declared != selected))
                throw std::invalid_argument("ambiguous/unconfigured finite sequence mode");
            declared = selected;
        }
    }
    if (declared && scenario != TestScenario::ActionReplay) {
        if (scenario != TestScenario::Configured && scenario != *declared) throw std::invalid_argument("CLI and INI mode disagree");
        scenario = *declared;
    }
    for (auto* node : nodes) {
        const auto active = cfg.sequenceModes.find(node->descriptor.id);
        if (active != cfg.sequenceModes.end() && active->second == TLSequenceMode::PASSIVE) continue;
        auto& group = node->descriptor.kind == TestNodeKind::Coherent ? coherent :
            node->descriptor.kind == TestNodeKind::Transient ? transient : mmio;
        group.push_back(node->descriptor.id);
    }
    if (scenario != TestScenario::Configured && scenario != TestScenario::Replay && scenario != TestScenario::ActionReplay &&
        (coherent.empty() || transient.empty()))
        throw std::invalid_argument("finite action mode needs configured coherent and transient nodes");
    if (cfg.unifiedSequenceEnable && scenario != TestScenario::Configured && scenario != TestScenario::ActionReplay)
        throw std::invalid_argument("finite and unified sequence modes conflict");
    if (scenario == TestScenario::Configured && cfg.unifiedSequenceEnable && cfg.unifiedSequenceMode == TLUnifiedSequenceMode::ANVIL) {
        const auto& a = cfg.unifiedSequenceModeAnvil;
        if (a.size < 8 || !a.widthB || coherent.size() < 1 + a.widthB || !a.epoch ||
            a.size > (cfg.memoryEnd - cfg.memoryStart) / 64) throw std::invalid_argument("invalid configured Anvil action topology/range");
        anvilOrder = coherent;
    }
    if (scenario == TestScenario::RandomRead && (coherent.size() < 2 || transient.size() < 2))
        throw std::invalid_argument("random-read mode needs two configured coherent and transient nodes");
    if (scenario == TestScenario::Actions && mmio.empty())
        throw std::invalid_argument("action mode needs a configured MMIO node");
    if (scenario == TestScenario::RandomActions) {
        const auto span = cfg.randomActionLines * 64;
        if (mmio.empty() || !cfg.randomActionEpisodes || cfg.randomActionEpisodes > 40 ||
            !cfg.randomActionLines || cfg.randomActionLines > 8 ||
            !cfg.memoryEnable || (cfg.memoryStart & 63) || cfg.memoryEnd < cfg.memoryStart ||
            span > cfg.memoryEnd - cfg.memoryStart || !cfg.mmioEnable || (cfg.mmioStart & 63) ||
            cfg.mmioEnd < cfg.mmioStart || span > cfg.mmioEnd - cfg.mmioStart ||
            !cfg.cmoEnable || !cfg.cmoEnableCBOClean || !cfg.cmoEnableCBOFlush || !cfg.cmoEnableCBOInval ||
            cfg.cmoStart > cfg.memoryStart || cfg.cmoEnd < cfg.memoryStart + span)
            throw std::invalid_argument("random-actions requires bounded episodes/lines and enabled aligned memory/CMO/MMIO regions");
    }
    if ((scenario == TestScenario::Replay || scenario == TestScenario::ActionReplay) && this->replay.empty())
        throw std::invalid_argument("empty action replay");
}
TestActionResult NodeFuzzer::submit(const PlannedTestAction& planned) {
    if (planned.node >= nodes.size()) throw std::invalid_argument("action selected an unconfigured node");
    const auto result = nodes[planned.node]->act(planned.action);
    if (result != TestActionResult::Accepted && rejected) rejected(planned, result);
    if (result == TestActionResult::Illegal) throw std::invalid_argument("node rejected illegal stimulus action");
    if (result == TestActionResult::Accepted && observed) observed(planned);
    return result;
}
PlannedTestAction NodeFuzzer::next() {
    if (scenario == TestScenario::Replay) return replay.at(issued);
    PlannedTestAction planned{transient.front(), {TestActionKind::ReadOnce, cfg.memoryStart}};
    auto& action = planned.action;
    action.txn = 20 + issued;
    action.traceTag = issued % 2 == 0;
    if (scenario == TestScenario::RandomRead) {
        const unsigned line = random() % 2;
        action.address += line * 64;
        if (random() % 2) {
            planned.node = coherent[line];
            action.kind = TestActionKind::AcquireUnique;
        } else planned.node = transient[random() % 2];
    } else if (scenario == TestScenario::Victim) {
        const unsigned line = issued < 12 ? issued : issued == 12 ? 0 : 9;
        action.address += uint64_t(line) * 0x20000;
        if (!issued) { planned.node = coherent.front(); action.kind = TestActionKind::AcquireUnique; }
    } else if (scenario == TestScenario::Actions) {
        // The same agent API drives permission, release, CMO and MMIO paths.
        static constexpr TestActionKind kinds[] = {TestActionKind::AcquireShared, TestActionKind::AcquireUnique,
            TestActionKind::Store, TestActionKind::WriteBack, TestActionKind::MakeUnique, TestActionKind::Clean,
            TestActionKind::Flush, TestActionKind::AcquireShared, TestActionKind::Evict,
            TestActionKind::ReadOnce, TestActionKind::AcquireUnique, TestActionKind::Store,
            TestActionKind::Invalidate, TestActionKind::ReadOnce, TestActionKind::WriteNoSnp, TestActionKind::ReadNoSnp};
        action.kind = kinds[issued];
        if (issued >= 4 && issued < 14) action.address += 128; // MakeUnique miss with explicit full-line overwrite.
        planned.node = issued == 9 || issued == 13 ? transient.front() : issued >= 14 ? mmio.front() : coherent.front();
        if (issued >= 14) action.address = cfg.mmioStart;
        action.size = issued >= 14 ? 3 : 6;
        for (unsigned i = 0; i < 64; ++i) action.data[i] = uint8_t(i * 17 ^ cfg.seed ^ (0xa5 + issued));
        if (issued >= 14) action.byteEnable = 0xff;
    } else {
        if (issued) action.address += 64;
        if (issued == 1) { planned.node = coherent.front(); action.kind = TestActionKind::AcquireShared; }
        if (issued == 2 || issued == 4) { planned.node = coherent.front(); action.kind = TestActionKind::AcquireUnique; }
    }
    return planned;
}
void NodeFuzzer::buildRandomSegment() {
    const auto address = cfg.memoryStart + uint64_t(random() % cfg.randomActionLines) * 64;
    const auto owner = coherent[random() % coherent.size()];
    auto add = [&](unsigned node, TestActionKind kind, uint64_t target) {
        TestAction action{kind, target, 20 + issued + unsigned(randomSegment.size()),
                          (issued + randomSegment.size()) % 2 == 0};
        for (unsigned i = 0; i < 64; ++i) action.data[i] = uint8_t(random());
        randomSegment.push_back({node, action});
    };
    // Each coherent segment releases its client before another owner is chosen.
    // Decisions use seed and accepted intent only, never DUT residency or bytes.
    switch (random() % 6) {
    case 0:
        add(transient[random() % transient.size()], TestActionKind::ReadOnce, address);
        break;
    case 1:
        add(owner, TestActionKind::AcquireShared, address);
        add(owner, TestActionKind::Clean, address);
        add(owner, TestActionKind::Evict, address);
        break;
    case 2: case 3: case 4: {
        add(owner, TestActionKind::AcquireUnique, address);
        add(owner, TestActionKind::Store, address);
        const auto release = random() % 4;
        add(owner, release == 0 ? TestActionKind::WriteBack : release == 1 ? TestActionKind::Clean :
            release == 2 ? TestActionKind::Flush : TestActionKind::Invalidate, address);
        if (release == 1) add(owner, TestActionKind::Evict, address);
        break;
    }
    default: {
        const auto node = mmio[random() % mmio.size()];
        const auto device = cfg.mmioStart + uint64_t(random() % cfg.randomActionLines) * 64;
        add(node, TestActionKind::WriteNoSnp, device);
        randomSegment.back().action.size = 3;
        randomSegment.back().action.byteEnable = 1 + random() % 255;
        add(node, TestActionKind::ReadNoSnp, device);
        randomSegment.back().action.size = 3;
        break;
    }
    }
}
void NodeFuzzer::unified() {
    if (cfg.unifiedSequenceMode == TLUnifiedSequenceMode::PASSIVE || !cfg.memoryEnable || !hooks.ready()) return;
    const auto& a = cfg.unifiedSequenceModeAnvil;
    const auto thresholdR = a.thresholdR ? a.thresholdR : a.size * 3 / 4;
    const auto thresholdB = a.thresholdB ? a.thresholdB : a.size * 3 / 4;
    auto offer = [&](unsigned node, TestActionKind kind, uint64_t index) {
        TestAction action{kind, cfg.memoryStart + index * 64, unsigned((20 + issued) % 256), issued % 2 == 0};
        for (unsigned i = 0; i < 64; ++i) action.data[i] = uint8_t((action.address >> 6) ^ (i * 17) ^ cfg.seed ^ anvilEpoch);
        if (submit({node, action}) != TestActionResult::Accepted) return false;
        ++issued; return true;
    };
    // Same U/R/B counters, configured thresholds, width and epoch permutation.
    // U is an explicit full-line overwrite intent; no protocol permission
    // operation is guessed from a TileLink opcode in this scheduler.
    if (anvilU < a.size && nodes[anvilOrder[0]]->idle()) { if (offer(anvilOrder[0], TestActionKind::MakeUnique, anvilU)) ++anvilU; }
    if (anvilR < a.size && anvilR < anvilU && anvilU >= thresholdR && nodes[anvilOrder[0]]->idle()) {
        if (offer(anvilOrder[0], TestActionKind::WriteBack, anvilR)) ++anvilR;
    }
    if (anvilB < a.size && anvilB < anvilR && anvilR >= thresholdB) {
        const auto node = anvilOrder[1 + anvilB % a.widthB];
        // A second persistent identity can remain Deferred on AnserL2.
        // Do not inject a hidden release or fabricate shared-owner support.
        if (offer(node, TestActionKind::AcquireShared, anvilB)) ++anvilB;
    }
    if (anvilU == a.size && anvilR == a.size && anvilB == a.size) {
        ++anvilEpoch; anvilU = anvilR = anvilB = 0;
        std::next_permutation(anvilOrder.begin(), anvilOrder.end());
    }
    if (a.noise && issued && !done()) {
        const auto node = coherent[random() % coherent.size()];
        offer(node, TestActionKind::AcquireUnique, random() % a.size);
    }
}
void NodeFuzzer::configured() {
    if (cfg.unifiedSequenceEnable) { unified(); return; }
    // Same ARI tag/set ranges, seed permutation, intervals and stream bounds as
    // CFuzzer. There is no wire protocol decision in this address scheduler.
    static constexpr uint64_t tags[] = {CFUZZER_RAND_RANGE_TAG, 1, 0x10}, sets[] = {CFUZZER_RAND_RANGE_SET, 0x10, 1};
    if (cfg.memoryEnd <= cfg.memoryStart) return;
    for (auto* node : nodes) {
        const unsigned id = node->descriptor.id;
        if (node->descriptor.kind != TestNodeKind::MMIO && !cfg.memoryEnable) continue;
        const auto found = cfg.sequenceModes.find(id);
        const auto mode = found == cfg.sequenceModes.end() ? TLSequenceMode::FUZZ_ARI : found->second;
        if (mode == TLSequenceMode::PASSIVE || !node->idle()) continue;
        uint64_t address;
        if (mode == TLSequenceMode::FUZZ_ARI) {
            std::array<unsigned, 3> order{0, 1, 2};
            for (unsigned i = 0; i < cfg.seed % 6; ++i) std::next_permutation(order.begin(), order.end());
            const auto interval = std::max<uint64_t>(1, cfg.fuzzARIInterval);
            const uint64_t iteration = *cycles / interval;
            if (cfg.fuzzARITarget && iteration / 3 >= cfg.fuzzARITarget) continue;
            for (unsigned i = 0; i < (iteration / 3) % 6; ++i) std::next_permutation(order.begin(), order.end());
            const auto range = order[iteration % 3];
            address = ((uint64_t(random()) % tags[range]) << 13) + ((uint64_t(random()) % sets[range]) << 6);
        } else if (mode == TLSequenceMode::FUZZ_STREAM || mode == TLSequenceMode::BWPROF_STREAM_STRIDE_READ) {
            if (*cycles < streamAt[id] || streamNext[id] >= cfg.fuzzStreamEnd) continue;
            address = streamNext[id];
        } else throw std::invalid_argument("configured action mode is unsupported; no approximate protocol transaction");
        TestAction action{node->descriptor.kind == TestNodeKind::Coherent ? TestActionKind::AcquireUnique :
            node->descriptor.kind == TestNodeKind::Transient ? TestActionKind::ReadOnce : TestActionKind::ReadNoSnp,
            mode == TLSequenceMode::FUZZ_ARI ? cfg.memoryStart + address % (cfg.memoryEnd - cfg.memoryStart) : address};
        if (node->descriptor.kind == TestNodeKind::MMIO) {
            if (!cfg.mmioEnable || cfg.mmioEnd <= cfg.mmioStart) continue;
            action.address = cfg.mmioStart + address % (cfg.mmioEnd - cfg.mmioStart);
            action.size = 3;
            action.address &= ~uint64_t(7);
        } else action.address &= ~uint64_t(63);
        action.txn = (20 + issued) % 256;
        action.traceTag = issued % 2 == 0;
        if (submit({id, action}) == TestActionResult::Accepted) {
            ++issued;
            if (mode != TLSequenceMode::FUZZ_ARI) {
                streamNext[id] += cfg.fuzzStreamStep;
                streamAt[id] = *cycles + cfg.fuzzStreamInterval;
            }
        }
    }
}
bool NodeFuzzer::done() const {
    if (scenario == TestScenario::RandomActions) return randomEpisode == cfg.randomActionEpisodes;
    if (scenario == TestScenario::Configured) {
        if (cfg.unifiedSequenceEnable) return cfg.unifiedSequenceMode == TLUnifiedSequenceMode::PASSIVE || anvilEpoch >= cfg.unifiedSequenceModeAnvil.epoch;
        for (const auto* node : nodes) {
            const auto mode = cfg.sequenceModes.find(node->descriptor.id);
            const auto kind = mode == cfg.sequenceModes.end() ? TLSequenceMode::FUZZ_ARI : mode->second;
            if (kind == TLSequenceMode::PASSIVE) continue;
            if (kind == TLSequenceMode::FUZZ_ARI) {
                if (!cfg.fuzzARITarget || *cycles / std::max<uint64_t>(1, cfg.fuzzARIInterval) < 3 * cfg.fuzzARITarget) return false;
            } else if (streamNext[node->descriptor.id] < cfg.fuzzStreamEnd) return false;
        }
        return true;
    }
    if (scenario == TestScenario::LowerSnoop) return issued == 5 && phase == 3;
    return issued == ((scenario == TestScenario::Replay || scenario == TestScenario::ActionReplay) ? replay.size() : scenario == TestScenario::RandomRead ? 96 :
        scenario == TestScenario::Victim ? 14 : scenario == TestScenario::Actions ? 16 : 4);
}
void NodeFuzzer::tick() {
    if (startupInterval < cfg.startupCycle) { ++startupInterval; return; }
    if (scenario == TestScenario::Configured) { if (!done()) configured(); return; }
    if (done() || !hooks.ready()) return;
    if (scenario == TestScenario::RandomActions) {
        if (randomSegment.empty()) buildRandomSegment();
        if (submit(randomSegment.at(randomStep)) == TestActionResult::Accepted) {
            ++issued;
            if (++randomStep == randomSegment.size()) {
                ++randomEpisode; randomStep = 0; randomSegment.clear();
            }
        }
        return;
    }
    if (scenario == TestScenario::Victim && issued == 1 && !stored ||
        scenario == TestScenario::LowerSnoop && issued == 5 && !stored) {
        const auto address = cfg.memoryStart + (scenario == TestScenario::LowerSnoop ? 64 : 0);
        TestAction action{TestActionKind::Store, address};
        for (unsigned i = 0; i < 64; ++i) action.data[i] = uint8_t((address >> 6) ^ (i * 17) ^ cfg.seed);
        action.data[0] ^= 0xa5;
        if (scenario == TestScenario::Victim) action.data[63] ^= 0x5a;
        if (submit({coherent.front(), action}) != TestActionResult::Accepted) return;
        stored = true;
    }
    if (scenario == TestScenario::LowerSnoop && issued == 5) {
        if (phase == 0) { hooks.lowerShared(cfg.memoryStart + 64); phase = 1; }
        else if (phase == 1 && hooks.lowerSharedComplete()) { hooks.lowerUniqueForward(cfg.memoryStart + 64); phase = 2; }
        else if (phase == 2 && hooks.lowerForwardComplete()) phase = 3;
        return;
    }
    // Hold a random choice on Deferred rather than selecting another node/line.
    if (replay.empty()) replay.push_back(next());
    const auto planned = (scenario == TestScenario::Replay || scenario == TestScenario::ActionReplay) ? replay.at(issued) : replay.front();
    if (submit(planned) == TestActionResult::Accepted) {
        ++issued;
        if (scenario != TestScenario::Replay && scenario != TestScenario::ActionReplay) replay.clear();
    }
}
