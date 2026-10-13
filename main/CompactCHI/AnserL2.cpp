#include "VL2Cache.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include "../Sequencer/TLSequencer.hpp"
#include "../System/TLConfig.hpp"
#include "../Events/TLSystemEvent.hpp"
#include "Requester.hpp"
#include "ActionReplay.hpp"
#include "EbHome.hpp"
#include "LowerSnoopBoard.hpp"
#include "CanonicalTrace.hpp"
#include "VictimProfile.hpp"
#include "NodeAgent.hpp"
#include "ResourceMonitor.hpp"
#include "DirectoryMonitor.hpp"
#include "ForwardNodeBoard.hpp"
#include "DataStorageMonitor.hpp"
#include <set>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <cstdio>

namespace {
class DutAdapter : public TestProtocolAdapter, public TLLocalContext, public TestScenarioHooks {
    VL2Cache& dut;
    VerilatedContext& simulation;
    TLLocalConfig* cfg = nullptr;
    uint64_t* cycles = nullptr;
    std::unique_ptr<compactchi::ArchitecturalBoard> board;
    std::unique_ptr<compactchi::Requester> requester;
    std::unique_ptr<chi_agent::HomeMemory> memory;
    std::unique_ptr<compactchi::EbHome> home;
    std::unique_ptr<compactchi::LowerSnoopBoard> lowerBoard;
    std::deque<compactchi::LowerSnoop> lowerQueue;
    bool lowerProfile, victimProfile;
    compactchi::VictimProfile victim;
    unsigned lowerPhase = 0;
    compactchi::LineData dirtyLine{};
    std::unique_ptr<compactchi::CanonicalTrace> trace;
    std::unique_ptr<compactchi::ResourceMonitor> resources;
    std::unique_ptr<compactchi::DirectoryMonitor> directory;
    std::unique_ptr<compactchi::DataStorageMonitor> dataStorage;
    compactchi::ForwardNodeBoard forwardNode;
    bool directoryScan = false, directoryChecked = false, dataStorageScan = false;
    compactchi::ResourceSnapshot resourceSnapshot() const {
        return {{dut.io_resources_mshrs, dut.io_resources_responses, dut.io_resources_writebacks,
            dut.io_resources_requests, dut.io_resources_queues, dut.io_resources_pipeline,
            dut.io_resources_serializers, dut.io_resources_controllers, dut.io_resources_retryOwners,
            dut.io_resources_credits}, bool(dut.io_resources_ready)};
    }
    std::mt19937 random;
    TestScenario scenario;
    bool mutateData;
    std::vector<PlannedTestAction> replay;
    NodeFuzzer* actionScheduler = nullptr;
    std::set<uint64_t> initializedLines;
    std::ofstream stimulus, actions, rejections, prefetchEvidence;
    unsigned quiet = 0, downRequests = 0;
    bool successful = false;
    std::filesystem::path output;

    compactchi::EbRequest downRequest() const {
        compactchi::EbRequest request{dut.io_downReq_bits_srcID, dut.io_downReq_bits_tgtID,
            dut.io_downReq_bits_txnID, dut.io_downReq_bits_opcode, dut.io_downReq_bits_addr,
            bool(dut.io_downReq_bits_traceTag), bool(dut.io_downReq_bits_expCompAck), dut.io_downReq_bits_size};
        request.allowRetry = dut.io_downReq_bits_allowRetry;
        request.pCrdType = dut.io_downReq_bits_pCrdType;
        request.memAttr = dut.io_downReq_bits_memAttr;
        request.order = dut.io_downReq_bits_order;
        request.ns = dut.io_downReq_bits_ns;
        request.likelyShared = dut.io_downReq_bits_likelyShared;
        return request;
    }
    void observe(compactchi::Channel channel, bool valid, bool ready, unsigned source,
                 unsigned target, unsigned txn, unsigned opcode, bool tag,
                 std::optional<uint64_t> address = std::nullopt,
                 std::optional<unsigned> dbid = std::nullopt,
                 std::optional<unsigned> dataId = std::nullopt,
                 std::optional<unsigned> resp = std::nullopt,
                 std::optional<unsigned> error = std::nullopt,
                 const uint32_t* data = nullptr) {
        if (!valid) return;
        compactchi::CanonicalEvent event{cycle(), simulation.time() - 1, channel, source, target, txn,
                                        opcode, valid, ready, tag};
        event.address = address;
        event.dbid = dbid;
        event.dataId = dataId;
        event.resp = resp;
        event.respErr = error;
        if (channel == compactchi::Channel::CchiTxDat) event.dataSource = dut.io_upDat_bits_dataSource;
        if (channel == compactchi::Channel::CchiRxDat) event.byteEnable = dut.io_upDatIn_bits_byteEnable;
        if (channel == compactchi::Channel::ChiTxDat) {
            event.forwardState = dut.io_downTxDat_bits_fwdState;
            event.homeNode = dut.io_downTxDat_bits_homeNID;
            event.byteEnable = dut.io_downTxDat_bits_byteEnable;
        }
        if (channel == compactchi::Channel::ChiTxRsp) event.forwardState = dut.io_downTxRsp_bits_fwdState;
        if (channel == compactchi::Channel::ChiTxReq) {
            const auto request = downRequest();
            event.allowRetry = request.allowRetry; event.expCompAck = request.expCompAck;
            event.pCrdType = request.pCrdType; event.size = request.size;
            event.memAttr = request.memAttr; event.order = request.order;
            event.ns = request.ns; event.likelyShared = request.likelyShared;
        }
        if (channel == compactchi::Channel::ChiRxRsp) event.pCrdType = dut.io_downRsp_bits_pCrdType;
        if (channel == compactchi::Channel::ChiTxRsp) event.pCrdType = dut.io_downTxRsp_bits_pCrdType;
        if (channel == compactchi::Channel::ChiRxDat) event.homeNode = dut.io_downDat_bits_homeNID;
        if (channel == compactchi::Channel::ChiRxSnp) {
            event.forwardNode = dut.io_downSnp_bits_fwdNID;
            event.forwardTxn = dut.io_downSnp_bits_fwdTxnID;
        }
        if (data) {
            event.data = std::array<uint8_t, 32>{};
            for (unsigned byte = 0; byte < 32; ++byte)
                (*event.data)[byte] = uint8_t(data[byte / 4] >> ((byte % 4) * 8));
        }
        trace->record(event);
    }
    bool ready() const override {
        const bool drained = !actionScheduler || (actionScheduler->getScenario() != TestScenario::RandomActions &&
            actionScheduler->getScenario() != TestScenario::ActionReplay) ||
            resourceSnapshot().empty();
        return requester->idle() && board->idle() && home->idle() && drained;
    }
    void lowerShared(uint64_t address) override {
        const compactchi::LowerSnoop snoop{2048, 1, address, false, true};
        lowerBoard->issue(snoop, compactchi::Permission::UD);
        lowerQueue.push_back(snoop); lowerPhase = 1;
    }
    bool lowerSharedComplete() override {
        const uint64_t address = cfg->memoryStart + 64;
        if (lowerPhase == 1 && lowerBoard->complete(2048) && home->snoopCommitReady()) {
            const auto result = lowerBoard->retire(2048);
            if (result.dutAfter != compactchi::Permission::SC || result.dirtySink != compactchi::DirtySink::Home)
                throw std::runtime_error("dirty lower Snoop responsibility mismatch");
            home->commitSnoopData(address, result.latest); lowerPhase = 2;
        }
        if (lowerPhase != 2 || !home->idle()) return false;
        for (unsigned i = 0; i < 64; ++i)
            if (memory->backend().access(address + i) != dirtyLine[i]) throw std::runtime_error("dirty Snoop memory mismatch");
        return true;
    }
    void lowerUniqueForward(uint64_t address) override {
        const compactchi::LowerSnoop snoop{2049, 0x17, address, false, true, 12, 2500};
        lowerBoard->issue(snoop, compactchi::Permission::SC); lowerQueue.push_back(snoop); lowerPhase = 3;
    }
    bool lowerForwardComplete() override {
        if (lowerPhase != 3 || !lowerBoard->complete(2049)) return false;
        const auto result = lowerBoard->retire(2049);
        if (result.dutAfter != compactchi::Permission::I || result.forwardAfter != compactchi::Permission::UC ||
            result.dirtySink != compactchi::DirtySink::None) throw std::runtime_error("forward state mismatch");
        lowerPhase = 4; return true;
    }
    void prepare(uint64_t address) {
        address &= ~uint64_t(63);
        if (!memory->backend().accessible(address) || !memory->backend().accessible(address + 63))
            throw std::invalid_argument("action/prefetch line outside configured memory");
        if (!initializedLines.insert(address).second) return;
        const auto bytes = compactchi::VictimProfile::initial(address, cfg->seed);
        for (unsigned i = 0; i < 64; ++i) memory->backend().access(address + i) = bytes[i];
        board->initialize(address, bytes);
    }
    bool terminalReady() const {
        return actionScheduler && actionScheduler->done() && ready() && lowerBoard->idle() &&
            (!victimProfile || victim.complete()) && forwardNode.idle() && resourceSnapshot().empty() &&
            !dut.io_downReq_valid && !dut.io_upSnp_valid;
    }
    void writeForwardEvidence() const {
        std::ofstream stream(output / "forward.jsonl");
        if (!stream) throw std::runtime_error("cannot open forward endpoint evidence");
        for (const auto& row : forwardNode.observations())
            stream << "{\"kind\":\"" << row.kind << "\",\"cycle\":" << row.cycle
                   << ",\"txn\":" << row.txn << ",\"dbid\":" << row.dbid
                   << ",\"source\":" << row.source << ",\"target\":" << row.target
                   << ",\"beat\":" << row.beat << ",\"trace_tag\":" << (row.traceTag ? "true" : "false") << "}\n";
        if (!stream) throw std::runtime_error("forward endpoint evidence write failed");
    }
    void maintain() {
        if (auto ack = forwardNode.tick(cycle()))
            lowerBoard->forwardCompAck(ack->source, ack->target, ack->txn, ack->traceTag);
        const bool drained = terminalReady();
        if (drained) ++quiet;
        else quiet = 0;
        if (!directoryScan && quiet == 20) {
            directoryScan = true; // Sample the first indexed row on the next real cycle.
            return;
        }
        if (directoryScan && directory->complete() && !directoryChecked) {
            compactchi::DirectoryClients expectedClients;
            for (const auto& [key, copy] : board->expectedTerminal().requesters)
                expectedClients.emplace(key, copy.permission == compactchi::Permission::SC ? 1U : 2U);
            directory->finish(expectedClients);
            directoryChecked = true;
            directoryScan = false;
            dataStorageScan = true;
            return;
        }
        if (dataStorageScan && dataStorage->complete()) {
            dataStorage->finish();
            writeForwardEvidence();
            resources->finish();
            if (victimProfile) {
                const auto expected = compactchi::VictimProfile::dirty(cfg->seed, cfg->memoryStart);
                for (unsigned i = 0; i < 64; ++i)
                    if (memory->backend().access(cfg->memoryStart + i) != expected[i]) throw std::runtime_error("victim memory mismatch");
                if (board->state(cfg->memoryStart).permission != compactchi::Permission::I) throw std::runtime_error("victim owner remained");
            }
            compactchi::ArchitecturalState actual;
            actual.requesters = requester->terminalCopies();
            actual.devices = home->terminalDevices();
            for (const auto address : initializedLines) {
                auto& data = actual.memory[address];
                for (unsigned i = 0; i < 64; ++i) data[i] = memory->backend().access(address + i);
            }
            std::ofstream terminal(output / "architectural-state.json");
            board->checkTerminal(actual, &terminal);
            terminal.close();
            if (!terminal) throw std::runtime_error("cannot close architectural state evidence");
            successful = true; TLSystemFinishEvent().Fire();
        }
    }
public:
    DutAdapter(VL2Cache& dut, VerilatedContext& simulation, const std::filesystem::path& output,
               TestScenario scenario, const std::string& replayPath, bool mutateData)
        : dut(dut), simulation(simulation), lowerProfile(scenario == TestScenario::LowerSnoop),
          victimProfile(scenario == TestScenario::Victim), scenario(scenario), mutateData(mutateData), output(output) {
        if (scenario == TestScenario::ActionReplay) {
            std::ifstream input(replayPath);
            if (!input) throw std::invalid_argument("action-replay requires a readable full-intent file");
            replay = compactchi::readActionReplay(input);
            std::filesystem::copy_file(replayPath, output / "replay-actions.txt", std::filesystem::copy_options::overwrite_existing);
        } else if (!replayPath.empty()) {
            std::ifstream input(replayPath);
            if (!input) throw std::invalid_argument("cannot open action replay file");
            uint64_t plannedCycle, address; unsigned source, txn, opcode, tag;
            while (input >> plannedCycle >> source >> txn >> opcode >> address >> tag) {
                TestActionKind kind;
                if (opcode == 3) kind = TestActionKind::ReadOnce;
                else if (opcode == 4) kind = TestActionKind::AcquireShared;
                else if (opcode == 16) kind = TestActionKind::AcquireUnique;
                else throw std::invalid_argument("unsupported replay opcode, never approximated");
                replay.push_back({source, {kind, address, txn, tag != 0}});
            }
            if (!input.eof() || replay.empty() || replay.size() > 256) throw std::invalid_argument("invalid replay");
            this->scenario = TestScenario::Replay;
        }
    }
    bool hasActionNodes() const override { return true; }
    std::unique_ptr<TestNode> createNode(TestNodeDescriptor descriptor) override {
        return std::make_unique<compactchi::NodeAgent>(descriptor, *cfg, *requester, *board,
            [this](uint64_t address) { prepare(address); });
    }
    std::unique_ptr<NodeFuzzer> createFuzzer(const TLLocalConfig& config, const std::vector<TestNode*>& nodes) override {
        auto fuzzer = std::make_unique<NodeFuzzer>(config, nodes, *this, scenario, replay);
        actionScheduler = fuzzer.get();
        victimProfile = fuzzer->getScenario() == TestScenario::Victim;
        lowerProfile = fuzzer->getScenario() == TestScenario::LowerSnoop;
        std::ofstream selected(output / "scenario.json"); selected << unsigned(fuzzer->getScenario());
        fuzzer->onRejected([this](const PlannedTestAction& planned, TestActionResult result) {
            rejections << "{\"cycle\":" << cycle() << ",\"node\":" << planned.node << ",\"kind\":" << unsigned(planned.action.kind)
                << ",\"address\":" << planned.action.address << ",\"result\":" << unsigned(result) << "}\n"; rejections.flush();
        });
        fuzzer->onAction([this](const PlannedTestAction& planned) {
            const auto& a = planned.action;
            if (a.kind == TestActionKind::Store) { dirtyLine = a.data; if (victimProfile) victim.dirtyStored = true; }
            actions << "{\"cycle\":" << cycle() << ",\"node\":" << planned.node << ",\"kind\":" << unsigned(a.kind)
                << ",\"address\":" << a.address << ",\"txn\":" << a.txn << ",\"size\":" << a.size
                << ",\"trace_tag\":" << (a.traceTag ? "true" : "false") << ",\"byte_enable\":" << a.byteEnable << ",\"data\":\"";
            for (const auto byte : a.data) actions << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
            actions << std::dec << "\"}\n"; actions.flush();
        });
        return fuzzer;
    }
    uint64_t cycle() const noexcept override { return *cycles; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return cfg->seed; }
    const TLLocalConfig& config() const noexcept override { return *cfg; }
    TLLocalConfig& config() noexcept override { return *cfg; }
    bool passed() const { return successful; }
    void initialize(TLLocalConfig& config, uint64_t& cycles, GlobalBoard<paddr_t>& global,
                    UncachedBoard<paddr_t>& uncached, axi_agent::MemoryAgent& backend,
                    axi_agent::Bundle<BEATSIZE_MEMORY>& memoryPort) override {
        cfg = &config; this->cycles = &cycles; random.seed(config.seed);
        stimulus.open(output / "stimulus.txt"); actions.open(output / "actions.jsonl"); rejections.open(output / "rejections.jsonl");
        if (!stimulus || !actions || !rejections) throw std::runtime_error("cannot open action evidence");
        prefetchEvidence.open(output / "prefetch.jsonl");
        if (!prefetchEvidence) throw std::runtime_error("cannot open prefetch evidence");
        const auto layout = TestNodeLayout(config);
        board = std::make_unique<compactchi::ArchitecturalBoard>(*this, layout.size(), global, uncached);
        requester = std::make_unique<compactchi::Requester>(*board, 1);
        requester->configureNodes(layout);
        memory = std::make_unique<chi_agent::HomeMemory>(config, cycles, backend, memoryPort);
        home = std::make_unique<compactchi::EbHome>(*memory, *this, global);
        home->setDeviceOracle([this](uint64_t address) { return board->deviceWriteExpectation(address); });
        home->onMemoryCommit([this](uint64_t address, const compactchi::LineData& bytes) { board->memoryCommitted(address, bytes); });
        lowerBoard = std::make_unique<compactchi::LowerSnoopBoard>(*this, global);
        trace = std::make_unique<compactchi::CanonicalTrace>(output / "canonical", config.seed);
        resources = std::make_unique<compactchi::ResourceMonitor>((output / "resources.jsonl").string());
        directory = std::make_unique<compactchi::DirectoryMonitor>((output / "directory.jsonl").string(),
            dut.io_directorySets, dut.io_directoryWays, dut.io_directorySlices, dut.io_directoryLineBytes, dut.io_directoryAddrBits);
        dataStorage = std::make_unique<compactchi::DataStorageMonitor>((output / "datastorage.jsonl").string(),
            dut.io_directorySets, dut.io_directoryWays, dut.io_directorySlices, dut.io_directoryLineBytes, dut.io_directoryAddrBits);
        victim = compactchi::VictimProfile(config.memoryStart, config.memoryEnd);
        std::ofstream topology(output / "topology.json");
        topology << "{\"coreCount\":" << config.coreCount << ",\"tlcPerCore\":" << config.masterCountPerCoreTLC
            << ",\"tlulPerCore\":" << config.masterCountPerCoreTLUL << ",\"memoryStart\":" << config.memoryStart
            << ",\"memoryEnd\":" << config.memoryEnd << ",\"mmioStart\":" << config.mmioStart
            << ",\"mmioEnd\":" << config.mmioEnd << ",\"seed\":" << config.seed << ",\"nodes\":[";
        for (unsigned i = 0; i < layout.size(); ++i) {
            const auto& d = layout[i];
            if (i) topology << ',';
            topology << "{\"id\":" << d.id << ",\"core\":" << d.core << ",\"slot\":" << d.slot
                << ",\"kind\":" << unsigned(d.kind) << ",\"mode\":"
                << unsigned(config.sequenceModes.count(d.id) ? config.sequenceModes.at(d.id) : TLSequenceMode::FUZZ_ARI) << '}';
        }
        topology << "]}";
    }
    void handleChannels() override {
        resources->sample(cycle(), simulation.time() - 1, resourceSnapshot());
        prefetchEvidence << "{\"cycle\":" << cycle() << ",\"sim_time\":" << simulation.time() - 1
            << ",\"trainingDropped\":" << unsigned(dut.io_prefetchTrainingDropped)
            << ",\"delayedTrainingDropped\":" << unsigned(dut.io_prefetchDelayedTrainingDropped) << ",\"train\":";
        if (dut.io_prefetchTraining_valid) {
            prefetchEvidence << "{\"address\":" << dut.io_prefetchTraining_bits_address
                << ",\"source\":" << unsigned(dut.io_prefetchTraining_bits_source)
                << ",\"needT\":" << unsigned(dut.io_prefetchTraining_bits_needT)
                << ",\"hit\":" << unsigned(dut.io_prefetchTraining_bits_hit)
                << ",\"prefetched\":" << unsigned(dut.io_prefetchTraining_bits_prefetched)
                << ",\"traceTag\":" << unsigned(dut.io_prefetchTraining_bits_traceTag) << '}';
        } else prefetchEvidence << "null";
        auto recordPrefetch = [&](const char* name, bool valid, uint64_t address, unsigned source, bool unique, bool tag) {
            prefetchEvidence << ",\"" << name << "\":";
            if (valid) prefetchEvidence << "{\"address\":" << address << ",\"source\":" << source
                << ",\"unique\":" << unsigned(unique) << ",\"traceTag\":" << unsigned(tag) << '}';
            else prefetchEvidence << "null";
        };
        recordPrefetch("issue", dut.io_prefetchIssued_valid, dut.io_prefetchIssued_bits_address,
            dut.io_prefetchIssued_bits_source, dut.io_prefetchIssued_bits_unique, dut.io_prefetchIssued_bits_traceTag);
        recordPrefetch("drop", dut.io_prefetchDropped_valid, dut.io_prefetchDropped_bits_address,
            dut.io_prefetchDropped_bits_source, dut.io_prefetchDropped_bits_unique, dut.io_prefetchDropped_bits_traceTag);
        prefetchEvidence << ",\"complete\":";
        if (dut.io_prefetchCompleted_valid) prefetchEvidence << "{\"address\":" << dut.io_prefetchCompleted_bits_address
            << ",\"source\":" << unsigned(dut.io_prefetchCompleted_bits_prefetchSource)
            << ",\"denied\":" << unsigned(dut.io_prefetchCompleted_bits_denied)
            << ",\"traceTag\":" << unsigned(dut.io_prefetchCompleted_bits_traceTag) << '}';
        else prefetchEvidence << "null";
        prefetchEvidence << "}\n";
        prefetchEvidence.flush();
        if (directoryScan) {
            using E = compactchi::DirectoryEntry;
            directory->sample(cycle(), simulation.time() - 1, terminalReady(), dut.io_directorySet, dut.io_directoryWay,
                std::array<E, 4>{{
                    {dut.io_directoryEntries_0_tag, dut.io_directoryEntries_0_state, dut.io_directoryEntries_0_clientState, dut.io_directoryEntries_0_clientID, bool(dut.io_directoryEntries_0_client)},
                    {dut.io_directoryEntries_1_tag, dut.io_directoryEntries_1_state, dut.io_directoryEntries_1_clientState, dut.io_directoryEntries_1_clientID, bool(dut.io_directoryEntries_1_client)},
                    {dut.io_directoryEntries_2_tag, dut.io_directoryEntries_2_state, dut.io_directoryEntries_2_clientState, dut.io_directoryEntries_2_clientID, bool(dut.io_directoryEntries_2_client)},
                    {dut.io_directoryEntries_3_tag, dut.io_directoryEntries_3_state, dut.io_directoryEntries_3_clientState, dut.io_directoryEntries_3_clientID, bool(dut.io_directoryEntries_3_client)}
                }});
        }
        if (dataStorageScan) {
            auto line = [](const auto& wide) {
                compactchi::DataStorageLine result{};
                for (unsigned word = 0; word < 16; ++word)
                    for (unsigned byte = 0; byte < 4; ++byte)
                        result[word * 4 + byte] = uint8_t(wide[word] >> (byte * 8));
                return result;
            };
            dataStorage->sample(cycle(), simulation.time() - 1, terminalReady(), dut.io_directorySet, dut.io_directoryWay,
                std::array<compactchi::DataStorageLine, 4>{{line(dut.io_dataStorageLines_0), line(dut.io_dataStorageLines_1),
                    line(dut.io_dataStorageLines_2), line(dut.io_dataStorageLines_3)}},
                std::array<bool, 4>{{bool(dut.io_dataStorageErrors_0), bool(dut.io_dataStorageErrors_1),
                    bool(dut.io_dataStorageErrors_2), bool(dut.io_dataStorageErrors_3)}});
        }
        using C = compactchi::Channel;
        observe(C::CchiRxReq, dut.io_req_valid, dut.io_req_ready, dut.io_req_bits_srcID,
            dut.io_req_bits_tgtID, dut.io_req_bits_txnID, dut.io_req_bits_opcode, dut.io_req_bits_traceTag, dut.io_req_bits_addr);
        observe(C::CchiTxDat, dut.io_upDat_valid, dut.io_upDat_ready, dut.io_upDat_bits_srcID,
            dut.io_upDat_bits_tgtID, dut.io_upDat_bits_txnID, dut.io_upDat_bits_opcode, dut.io_upDat_bits_traceTag,
            std::nullopt, dut.io_upDat_bits_dbID, dut.io_upDat_bits_dataID, dut.io_upDat_bits_resp,
            dut.io_upDat_bits_respErr, dut.io_upDat_bits_data.data());
        observe(C::CchiTxSnp, dut.io_upSnp_valid, dut.io_upSnp_ready, dut.io_upSnp_bits_srcID,
            dut.io_upSnp_bits_tgtID, dut.io_upSnp_bits_txnID, dut.io_upSnp_bits_opcode, dut.io_upSnp_bits_traceTag,
            uint64_t(dut.io_upSnp_bits_addr) << 3);
        observe(C::CchiRxRsp, dut.io_upRspIn_valid, dut.io_upRspIn_ready, dut.io_upRspIn_bits_srcID,
            dut.io_upRspIn_bits_tgtID, dut.io_upRspIn_bits_txnID, dut.io_upRspIn_bits_opcode, dut.io_upRspIn_bits_traceTag,
            std::nullopt, std::nullopt, std::nullopt, dut.io_upRspIn_bits_resp, dut.io_upRspIn_bits_respErr);
        observe(C::ChiTxReq, dut.io_downReq_valid, dut.io_downReq_ready, dut.io_downReq_bits_srcID,
            dut.io_downReq_bits_tgtID, dut.io_downReq_bits_txnID, dut.io_downReq_bits_opcode, dut.io_downReq_bits_traceTag,
            dut.io_downReq_bits_addr);
        observe(C::ChiRxRsp, dut.io_downRsp_valid, dut.io_downRsp_ready, dut.io_downRsp_bits_srcID,
            dut.io_downRsp_bits_tgtID, dut.io_downRsp_bits_txnID, dut.io_downRsp_bits_opcode, dut.io_downRsp_bits_traceTag,
            std::nullopt, dut.io_downRsp_bits_dbID, std::nullopt, dut.io_downRsp_bits_resp, dut.io_downRsp_bits_respErr);
        observe(C::ChiRxDat, dut.io_downDat_valid, dut.io_downDat_ready, dut.io_downDat_bits_srcID,
            dut.io_downDat_bits_tgtID, dut.io_downDat_bits_txnID, dut.io_downDat_bits_opcode, dut.io_downDat_bits_traceTag,
            std::nullopt, dut.io_downDat_bits_dbID, dut.io_downDat_bits_dataID, dut.io_downDat_bits_resp,
            dut.io_downDat_bits_respErr, dut.io_downDat_bits_data.data());
        observe(C::ChiTxRsp, dut.io_downTxRsp_valid, dut.io_downTxRsp_ready, dut.io_downTxRsp_bits_srcID,
            dut.io_downTxRsp_bits_tgtID, dut.io_downTxRsp_bits_txnID, dut.io_downTxRsp_bits_opcode, dut.io_downTxRsp_bits_traceTag,
            std::nullopt, dut.io_downTxRsp_bits_dbID, std::nullopt, dut.io_downTxRsp_bits_resp, dut.io_downTxRsp_bits_respErr);
        observe(C::CchiRxEvt, dut.io_evt_valid, dut.io_evt_ready, dut.io_evt_bits_srcID,
            dut.io_evt_bits_tgtID, dut.io_evt_bits_txnID, dut.io_evt_bits_opcode, dut.io_evt_bits_traceTag, dut.io_evt_bits_addr);
        observe(C::CchiTxRsp, dut.io_upRsp_valid, dut.io_upRsp_ready, dut.io_upRsp_bits_srcID,
            dut.io_upRsp_bits_tgtID, dut.io_upRsp_bits_txnID, dut.io_upRsp_bits_opcode, dut.io_upRsp_bits_traceTag,
            std::nullopt, dut.io_upRsp_bits_dbID, std::nullopt, dut.io_upRsp_bits_resp, dut.io_upRsp_bits_respErr);
        observe(C::CchiRxDat, dut.io_upDatIn_valid, dut.io_upDatIn_ready, dut.io_upDatIn_bits_srcID,
            dut.io_upDatIn_bits_tgtID, dut.io_upDatIn_bits_txnID, dut.io_upDatIn_bits_opcode, dut.io_upDatIn_bits_traceTag,
            std::nullopt, std::nullopt, dut.io_upDatIn_bits_dataID, dut.io_upDatIn_bits_resp,
            dut.io_upDatIn_bits_respErr, dut.io_upDatIn_bits_data.data());
        observe(C::ChiRxSnp, dut.io_downSnp_valid, dut.io_downSnp_ready, dut.io_downSnp_bits_srcID,
            1, dut.io_downSnp_bits_txnID, dut.io_downSnp_bits_opcode, dut.io_downSnp_bits_traceTag,
            uint64_t(dut.io_downSnp_bits_addr) << 3);
        observe(C::ChiTxDat, dut.io_downTxDat_valid, dut.io_downTxDat_ready, dut.io_downTxDat_bits_srcID,
            dut.io_downTxDat_bits_tgtID, dut.io_downTxDat_bits_txnID, dut.io_downTxDat_bits_opcode, dut.io_downTxDat_bits_traceTag,
            std::nullopt, dut.io_downTxDat_bits_dbID, dut.io_downTxDat_bits_dataID, dut.io_downTxDat_bits_resp,
            dut.io_downTxDat_bits_respErr, dut.io_downTxDat_bits_data.data());
        if (dut.io_req_valid && dut.io_req_ready) {
            const auto& f = requester->request();
            stimulus << cycle() << ' ' << f.node << ' ' << f.txn << ' ' << f.opcode << ' ' << f.address << ' ' << f.traceTag << '\n';
            stimulus.flush(); requester->requestAccepted();
        }
        if (dut.io_evt_valid && dut.io_evt_ready) requester->eventAccepted();
        if (dut.io_upRspIn_valid && dut.io_upRspIn_ready) requester->responseAccepted();
        if (dut.io_upDatIn_valid && dut.io_upDatIn_ready) requester->dataAccepted();
        if (dut.io_downRsp_valid && dut.io_downRsp_ready) home->responseAccepted();
        if (dut.io_downDat_valid && dut.io_downDat_ready) home->dataAccepted();
        if (dut.io_upDat_valid && dut.io_upDat_ready) {
            compactchi::DataFlit flit{dut.io_upDat_bits_srcID, dut.io_upDat_bits_tgtID, dut.io_upDat_bits_txnID,
                dut.io_upDat_bits_dbID, dut.io_upDat_bits_dataID, dut.io_upDat_bits_resp, bool(dut.io_upDat_bits_traceTag)};
            flit.opcode = dut.io_upDat_bits_opcode;
            flit.respErr = dut.io_upDat_bits_respErr;
            for (unsigned b = 0; b < 32; ++b) flit.data[b] = uint8_t(dut.io_upDat_bits_data[b / 4] >> ((b % 4) * 8));
            // Explicit harness mutation for checking the integrated board/reducer,
            // never enabled by ordinary DUT acceptance runs.
            if (mutateData) { flit.data[0] ^= 1; mutateData = false; }
            requester->receiveData(flit);
        }
        if (dut.io_upSnp_valid && dut.io_upSnp_ready)
            requester->receiveSnoop({dut.io_upSnp_bits_srcID, dut.io_upSnp_bits_tgtID, dut.io_upSnp_bits_txnID,
                dut.io_upSnp_bits_opcode, uint64_t(dut.io_upSnp_bits_addr) << 3, bool(dut.io_upSnp_bits_traceTag)});
        if (dut.io_downSnp_valid && dut.io_downSnp_ready) lowerQueue.pop_front();
        if (dut.io_upRsp_valid && dut.io_upRsp_ready)
            requester->receiveResponse({dut.io_upRsp_bits_srcID, dut.io_upRsp_bits_tgtID, dut.io_upRsp_bits_txnID,
                dut.io_upRsp_bits_opcode, dut.io_upRsp_bits_resp, bool(dut.io_upRsp_bits_traceTag)},
                dut.io_upRsp_bits_dbID, dut.io_upRsp_bits_respErr);
        if (dut.io_downReq_valid && dut.io_downReq_ready) {
            compactchi::HomePlan plan;
            plan.separated = (++downRequests % 2) == 0;
            plan.completionAfterData = plan.separated;
            plan.upperBeatFirst = plan.separated;
            plan.completionSource = 14;
            plan.retryOnce = downRequests % 3 == 0;
            plan.creditBeforeRetry = plan.retryOnce;
            plan.responseTraceTag = downRequests % 2 == 0;
            // The Home context applies retryOnce only to a first request.
            const auto request = downRequest();
            if (request.opcode != 4 && request.opcode != 0x1c && request.opcode != 0x1d) prepare(request.address);
            if (victimProfile) plan = victim.observe(request);
            home->accept(request, plan);
        }
        if (dut.io_downTxRsp_valid && dut.io_downTxRsp_ready) {
            if (dut.io_downTxRsp_bits_opcode == 2)
                home->receiveCompAck(dut.io_downTxRsp_bits_srcID, dut.io_downTxRsp_bits_tgtID,
                    dut.io_downTxRsp_bits_txnID, dut.io_downTxRsp_bits_traceTag);
            else {
                compactchi::EbResponse response{dut.io_downTxRsp_bits_srcID, dut.io_downTxRsp_bits_tgtID,
                    dut.io_downTxRsp_bits_txnID, dut.io_downTxRsp_bits_opcode, dut.io_downTxRsp_bits_dbID,
                    dut.io_downTxRsp_bits_resp, bool(dut.io_downTxRsp_bits_traceTag)};
                response.fwdState = dut.io_downTxRsp_bits_fwdState;
                response.respErr = dut.io_downTxRsp_bits_respErr;
                lowerBoard->response(response);
            }
        }
        if (dut.io_downTxDat_valid && dut.io_downTxDat_ready) {
            compactchi::EbData data{dut.io_downTxDat_bits_srcID, dut.io_downTxDat_bits_tgtID,
                dut.io_downTxDat_bits_txnID, dut.io_downTxDat_bits_opcode, dut.io_downTxDat_bits_dbID,
                dut.io_downTxDat_bits_homeNID, dut.io_downTxDat_bits_dataID, dut.io_downTxDat_bits_resp,
                bool(dut.io_downTxDat_bits_traceTag)};
            data.fwdState = dut.io_downTxDat_bits_fwdState;
            data.respErr = dut.io_downTxDat_bits_respErr;
            data.byteEnable = dut.io_downTxDat_bits_byteEnable;
            for (unsigned b = 0; b < 32; ++b) data.data[b] = uint8_t(dut.io_downTxDat_bits_data[b / 4] >> ((b % 4) * 8));
            if (data.opcode == 3) home->receiveDeviceData(data);
            else if (data.opcode == 2) home->receiveCopyback(data);
            else if (data.target == 12) {
                lowerBoard->forwardData(data);
                forwardNode.accept(data, cycle());
            } else lowerBoard->homeData(data);
        }
        memory->handleChannels();
        home->service();
    }
    void fuzz() override { maintain(); }
    void updateSignals() override { memory->updateSignals(); }
    void drive() {
        dut.io_directorySet = (directoryScan || dataStorageScan) ? (directoryScan ? directory->set() : dataStorage->set()) : 0;
        dut.io_directoryWay = (directoryScan || dataStorageScan) ? (directoryScan ? directory->way() : dataStorage->way()) : 0;
        dut.io_l2NodeId = 1; dut.io_homeNodeId = 8;
        dut.io_flush = 0; dut.io_prefetchTask_valid = 0;
        dut.io_evt_valid = requester->eventValid();
        if (requester->eventValid()) {
            const auto& f = requester->event();
            dut.io_evt_bits_srcID = f.node; dut.io_evt_bits_tgtID = f.target; dut.io_evt_bits_txnID = f.txn;
            dut.io_evt_bits_opcode = f.opcode; dut.io_evt_bits_addr = f.address;
            dut.io_evt_bits_wayValid = 0; dut.io_evt_bits_way = 0; dut.io_evt_bits_traceTag = f.traceTag;
        }
        dut.io_downSnp_valid = !lowerQueue.empty();
        if (!lowerQueue.empty()) {
            const auto& snp = lowerQueue.front();
            dut.io_downSnp_bits_srcID = 8; dut.io_downSnp_bits_txnID = snp.txn;
            dut.io_downSnp_bits_opcode = snp.opcode; dut.io_downSnp_bits_addr = snp.address >> 3;
            dut.io_downSnp_bits_retToSrc = snp.retToSrc; dut.io_downSnp_bits_traceTag = snp.traceTag;
            dut.io_downSnp_bits_fwdNID = snp.forwardNode; dut.io_downSnp_bits_fwdTxnID = snp.forwardTxn;
            dut.io_downSnp_bits_ns = 0;
        }
        dut.io_upDatIn_valid = requester->dataValid();
        if (requester->dataValid()) {
            const auto& f = requester->data();
            dut.io_upDatIn_bits_srcID = f.node; dut.io_upDatIn_bits_tgtID = f.target;
            dut.io_upDatIn_bits_txnID = f.txn; dut.io_upDatIn_bits_opcode = f.opcode;
            dut.io_upDatIn_bits_dataID = f.dataId; dut.io_upDatIn_bits_resp = f.resp;
            dut.io_upDatIn_bits_respErr = 0; dut.io_upDatIn_bits_traceTag = f.traceTag;
            dut.io_upDatIn_bits_byteEnable = f.byteEnable;
            for (unsigned w = 0; w < 8; ++w) {
                uint32_t value = 0;
                for (unsigned b = 0; b < 4; ++b) value |= uint32_t(f.data[w * 4 + b]) << (b * 8);
                dut.io_upDatIn_bits_data[w] = value;
            }
        }
        dut.io_req_valid = requester->requestValid();
        if (requester->requestValid()) {
            const auto& f = requester->request();
            dut.io_req_bits_srcID = f.node; dut.io_req_bits_tgtID = f.target; dut.io_req_bits_txnID = f.txn;
            dut.io_req_bits_opcode = f.opcode; dut.io_req_bits_addr = f.address; dut.io_req_bits_size = f.size;
            dut.io_req_bits_tagAlias = 0; dut.io_req_bits_ns = 0; dut.io_req_bits_order = 0;
            dut.io_req_bits_memAttr = 0; dut.io_req_bits_excl = 0; dut.io_req_bits_expCompData = f.opcode == 2 || f.opcode == 3 || f.opcode == 4 || f.opcode == 16;
            dut.io_req_bits_expCompStash = 0; dut.io_req_bits_wayValid = 0; dut.io_req_bits_way = 0;
            dut.io_req_bits_traceTag = f.traceTag; dut.io_vaddr = f.address;
        }
        dut.io_upRspIn_valid = requester->responseValid();
        if (requester->responseValid()) {
            const auto& f = requester->response();
            dut.io_upRspIn_bits_srcID = f.node; dut.io_upRspIn_bits_tgtID = f.target; dut.io_upRspIn_bits_txnID = f.txn;
            dut.io_upRspIn_bits_opcode = f.opcode; dut.io_upRspIn_bits_resp = f.resp;
            dut.io_upRspIn_bits_traceTag = f.traceTag; dut.io_upRspIn_bits_respErr = 0;
        }
        dut.io_downRsp_valid = home->responseValid();
        if (home->responseValid()) {
            const auto& f = home->response();
            dut.io_downRsp_bits_srcID = f.source; dut.io_downRsp_bits_tgtID = f.target; dut.io_downRsp_bits_txnID = f.txn;
            dut.io_downRsp_bits_opcode = f.opcode; dut.io_downRsp_bits_dbID = f.dbid;
            dut.io_downRsp_bits_resp = f.resp; dut.io_downRsp_bits_respErr = 0; dut.io_downRsp_bits_pCrdType = f.pCrdType;
            dut.io_downRsp_bits_cBusy = 0; dut.io_downRsp_bits_traceTag = f.traceTag;
        }
        dut.io_downDat_valid = home->dataValid();
        if (home->dataValid()) {
            const auto& f = home->datum();
            dut.io_downDat_bits_srcID = f.source; dut.io_downDat_bits_tgtID = f.target; dut.io_downDat_bits_txnID = f.txn;
            dut.io_downDat_bits_opcode = f.opcode; dut.io_downDat_bits_dbID = f.dbid; dut.io_downDat_bits_homeNID = f.home;
            dut.io_downDat_bits_dataID = f.dataId; dut.io_downDat_bits_resp = f.resp; dut.io_downDat_bits_respErr = 0; dut.io_downDat_bits_traceTag = f.traceTag;
            for (unsigned w = 0; w < 8; ++w) {
                uint32_t value = 0;
                for (unsigned b = 0; b < 4; ++b) value |= uint32_t(f.data[w * 4 + b]) << (b * 8);
                dut.io_downDat_bits_data[w] = value;
            }
        }
        dut.io_upDat_ready = random() % 4 != 0; dut.io_upRsp_ready = random() % 4 != 0;
        dut.io_upSnp_ready = requester->snoopReady(); dut.io_downTxRsp_ready = random() % 4 != 0;
        dut.io_downTxDat_ready = random() % 4 != 0;
        dut.io_downReq_ready = 0;
        dut.eval();
        dut.io_downReq_ready = home->requestReady(downRequest());
        dut.eval();
    }
    void finalize() override {
        if (successful && (!requester->idle() || !board->idle() || !home->idle()))
            throw std::runtime_error("final DUT profile resources did not drain");
        if (successful) resources->finish();
        resources.reset(); directory.reset(); dataStorage.reset();
        trace.reset(); lowerBoard.reset(); home.reset(); memory.reset(); requester.reset(); board.reset();
    }
};
}

int main(int argc, char** argv) {
    try {
        const unsigned seed = argc > 1 ? std::stoul(argv[1]) : 42;
        if (!seed) throw std::invalid_argument("seed must be nonzero");
        const std::string profile = argc > 3 ? argv[3] : "configured";
        TestScenario scenario;
        if (profile == "configured") scenario = TestScenario::Configured;
        else if (profile == "deterministic-read") scenario = TestScenario::DeterministicRead;
        else if (profile == "random-read") scenario = TestScenario::RandomRead;
        else if (profile == "lower-snoop") scenario = TestScenario::LowerSnoop;
        else if (profile == "victim") scenario = TestScenario::Victim;
        else if (profile == "node-actions") scenario = TestScenario::Actions;
        else if (profile == "random-actions") scenario = TestScenario::RandomActions;
        else if (profile == "action-replay") scenario = TestScenario::ActionReplay;
        else throw std::invalid_argument("unknown action fuzzer mode");
        const auto output = std::filesystem::path(argc > 2 ? argv[2] : "build/compactchi-native") / std::to_string(seed);
        std::filesystem::create_directories(output);
        const std::string ini = argc > 6 ? argv[6] : "tltest.ini";
        auto config = LoadTestConfig([&](TLLocalConfig& config) { config.seed = seed; }, ini);
        std::filesystem::copy_file(ini, output / "tltest.ini", std::filesystem::copy_options::overwrite_existing);
        VerilatedContext simulation;
        simulation.commandArgs(argc, argv);
        simulation.traceEverOn(true);
        VL2Cache dut(&simulation);
        VerilatedVcdC waveform;
        dut.trace(&waveform, 99);
        waveform.open((output / "waveform.vcd").c_str());
        auto edge = [&](bool clock) { dut.clock = clock; dut.eval(); waveform.dump(simulation.time()); simulation.timeInc(1); };
        dut.reset = 1;
        dut.io_req_valid = dut.io_evt_valid = dut.io_prefetchTask_valid = 0;
        dut.io_upRspIn_valid = dut.io_upDatIn_valid = dut.io_downRsp_valid = dut.io_downDat_valid = dut.io_downSnp_valid = 0;
        dut.io_flush = 0;
        dut.io_directorySet = dut.io_directoryWay = 0;
        for (unsigned i = 0; i < 8; ++i) { edge(false); edge(true); }
        dut.reset = 0;
        const std::string replay = argc > 4 && std::string(argv[4]) != "-" ? argv[4] : "";
        const bool mutation = argc > 5 && std::string(argv[5]) == "data-error";
        DutAdapter adapter(dut, simulation, output, scenario, replay, mutation);
        TLSequencer sequencer;
        sequencer.Initialize(config, adapter);
        uint64_t cycle = 8;
        const uint64_t limit = config.maximumCycle ? config.maximumCycle : 10000;
        while (sequencer.IsAlive() && cycle < limit) {
            sequencer.Tick(cycle);
            dut.clock = 0;
            adapter.drive();
            waveform.dump(simulation.time());
            simulation.timeInc(1);
            sequencer.Tock();
            edge(true);
            ++cycle;
        }
        const bool passed = sequencer.IsFinished() && adapter.passed();
        sequencer.Finalize();
        dut.final(); waveform.close();
        std::printf("profile=%s seed=%llu cycles=%llu independent_board=%s trace=%s\n",
                    profile.c_str(), (unsigned long long)config.seed, (unsigned long long)cycle, passed ? "passed" : "failed", output.c_str());
        return passed && !sequencer.IsFailed() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
