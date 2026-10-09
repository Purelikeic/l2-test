#include "VL2Cache.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include "../Sequencer/TLSequencer.hpp"
#include "../System/TLConfig.hpp"
#include "../Events/TLSystemEvent.hpp"
#include "Requester.hpp"
#include "EbHome.hpp"
#include "CanonicalTrace.hpp"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <cstdio>

namespace {
class DutAdapter : public TestProtocolAdapter, public TLLocalContext {
    VL2Cache& dut;
    VerilatedContext& simulation;
    TLLocalConfig* cfg = nullptr;
    uint64_t* cycles = nullptr;
    std::unique_ptr<compactchi::ArchitecturalBoard> board;
    std::unique_ptr<compactchi::Requester> requester;
    std::unique_ptr<chi_agent::HomeMemory> memory;
    std::unique_ptr<compactchi::EbHome> home;
    std::unique_ptr<compactchi::CanonicalTrace> trace;
    std::mt19937 random;
    unsigned issued = 0, quiet = 0, downRequests = 0;
    bool successful = false;
    std::filesystem::path output;
    class ReadFuzzer : public Fuzzer {
        DutAdapter& owner;
    public:
        explicit ReadFuzzer(DutAdapter& owner) : owner(owner) {}
        void tick() override { owner.generate(); }
    } fuzzer{*this};
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
        if (data) {
            event.data = std::array<uint8_t, 32>{};
            for (unsigned byte = 0; byte < 32; ++byte)
                (*event.data)[byte] = uint8_t(data[byte / 4] >> ((byte % 4) * 8));
        }
        trace->record(event);
    }
    void generate() {
        // First deterministic profile: distinct transient/coherent identities,
        // one persistent client, known lines, and serialized read obligations.
        // This is not yet an N-node random transaction generator.
        static constexpr unsigned opcodes[] = {3, 4, 16, 3};
        static constexpr unsigned nodes[] = {0, 1, 1, 0};
        static constexpr uint64_t addresses[] = {0x1000, 0x1040, 0x1040, 0x1040};
        if (issued < 4 && requester->idle() && board->idle() && home->idle()) {
            requester->enqueue({nodes[issued], 1, 20 + issued, opcodes[issued], addresses[issued], false});
            ++issued;
        }
        if (issued == 4 && requester->idle() && board->idle() && home->idle() &&
            !dut.io_downReq_valid && !dut.io_upSnp_valid) ++quiet;
        else quiet = 0;
        if (quiet == 20) {
            successful = true;
            TLSystemFinishEvent().Fire();
        }
    }
public:
    DutAdapter(VL2Cache& dut, VerilatedContext& simulation, const std::filesystem::path& output)
        : dut(dut), simulation(simulation), output(output) {}
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
        if (config.GetAgentCount() < 2 || config.memoryStart > 0x1000 || config.memoryEnd < 0x2000)
            throw std::invalid_argument("deterministic DUT profile needs two identities and memory covering 0x1000..0x1fff");
        cfg = &config;
        this->cycles = &cycles;
        random.seed(config.seed);
        fuzzer.set_cycles(&cycles);
        board = std::make_unique<compactchi::ArchitecturalBoard>(*this, config.GetAgentCount(), global, uncached);
        requester = std::make_unique<compactchi::Requester>(*board, 1);
        memory = std::make_unique<chi_agent::HomeMemory>(config, cycles, backend, memoryPort);
        home = std::make_unique<compactchi::EbHome>(*memory, *this, global);
        trace = std::make_unique<compactchi::CanonicalTrace>(output / "canonical", config.seed);
        for (uint64_t address = 0x1000; address < 0x2000; address += 64) {
            compactchi::LineData bytes{};
            for (unsigned i = 0; i < 64; ++i) {
                bytes[i] = uint8_t((address >> 6) ^ (i * 17) ^ config.seed);
                backend.access(address + i) = bytes[i];
            }
            board->initialize(address, bytes);
        }
    }
    void handleChannels() override {
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
            dut.io_downRsp_bits_tgtID, dut.io_downRsp_bits_txnID, dut.io_downRsp_bits_opcode, false,
            std::nullopt, dut.io_downRsp_bits_dbID, std::nullopt, dut.io_downRsp_bits_resp, dut.io_downRsp_bits_respErr);
        observe(C::ChiRxDat, dut.io_downDat_valid, dut.io_downDat_ready, dut.io_downDat_bits_srcID,
            dut.io_downDat_bits_tgtID, dut.io_downDat_bits_txnID, dut.io_downDat_bits_opcode, false,
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
        if (dut.io_req_valid && dut.io_req_ready) requester->requestAccepted();
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
            requester->receiveData(flit);
        }
        if (dut.io_upSnp_valid && dut.io_upSnp_ready)
            requester->receiveSnoop({dut.io_upSnp_bits_srcID, dut.io_upSnp_bits_tgtID, dut.io_upSnp_bits_txnID,
                dut.io_upSnp_bits_opcode, uint64_t(dut.io_upSnp_bits_addr) << 3, bool(dut.io_upSnp_bits_traceTag)});
        if (dut.io_upRsp_valid) throw std::runtime_error("unsupported no-data completion in the initial read profile");
        if (dut.io_downReq_valid && dut.io_downReq_ready) {
            compactchi::HomePlan plan;
            plan.separated = (++downRequests % 2) == 0;
            plan.completionAfterData = plan.separated;
            plan.upperBeatFirst = plan.separated;
            plan.completionSource = 14;
            plan.retryOnce = downRequests % 3 == 0;
            plan.creditBeforeRetry = plan.retryOnce;
            // The Home context applies retryOnce only to a first request.
            home->accept(downRequest(), plan);
        }
        if (dut.io_downTxRsp_valid && dut.io_downTxRsp_ready) {
            if (dut.io_downTxRsp_bits_opcode != 2) throw std::runtime_error("unsupported lower Snoop response in initial read profile");
            home->receiveCompAck(dut.io_downTxRsp_bits_srcID, dut.io_downTxRsp_bits_tgtID,
                dut.io_downTxRsp_bits_txnID, dut.io_downTxRsp_bits_traceTag);
        }
        if (dut.io_downTxDat_valid) throw std::runtime_error("unexpected lower copyback in initial read profile");
        memory->handleChannels();
        home->service();
    }
    void fuzz() override { fuzzer.tick(); }
    void updateSignals() override { memory->updateSignals(); }
    void drive() {
        dut.io_l2NodeId = 1; dut.io_homeNodeId = 8;
        dut.io_flush = 0; dut.io_evt_valid = 0; dut.io_prefetchTask_valid = 0; dut.io_downSnp_valid = 0;
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
            dut.io_req_bits_opcode = f.opcode; dut.io_req_bits_addr = f.address; dut.io_req_bits_size = 6;
            dut.io_req_bits_tagAlias = 0; dut.io_req_bits_ns = 0; dut.io_req_bits_order = 0;
            dut.io_req_bits_memAttr = 0; dut.io_req_bits_excl = 0; dut.io_req_bits_expCompData = 1;
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
            dut.io_downRsp_bits_cBusy = 0;
        }
        dut.io_downDat_valid = home->dataValid();
        if (home->dataValid()) {
            const auto& f = home->datum();
            dut.io_downDat_bits_srcID = f.source; dut.io_downDat_bits_tgtID = f.target; dut.io_downDat_bits_txnID = f.txn;
            dut.io_downDat_bits_opcode = f.opcode; dut.io_downDat_bits_dbID = f.dbid; dut.io_downDat_bits_homeNID = f.home;
            dut.io_downDat_bits_dataID = f.dataId; dut.io_downDat_bits_resp = f.resp; dut.io_downDat_bits_respErr = 0;
            for (unsigned w = 0; w < 8; ++w) {
                uint32_t value = 0;
                for (unsigned b = 0; b < 4; ++b) value |= uint32_t(f.data[w * 4 + b]) << (b * 8);
                dut.io_downDat_bits_data[w] = value;
            }
        }
        dut.io_upDat_ready = random() % 4 != 0; dut.io_upRsp_ready = 1;
        dut.io_upSnp_ready = requester->snoopReady(); dut.io_downTxRsp_ready = random() % 4 != 0;
        dut.io_downTxDat_ready = 1;
        dut.io_downReq_ready = 0;
        dut.eval();
        dut.io_downReq_ready = home->requestReady(downRequest());
        dut.eval();
    }
    void finalize() override {
        if (successful && (!requester->idle() || !board->idle() || !home->idle()))
            throw std::runtime_error("final DUT profile resources did not drain");
        trace.reset(); home.reset(); memory.reset(); requester.reset(); board.reset();
    }
};
}

int main(int argc, char** argv) {
    try {
        const unsigned seed = argc > 1 ? std::stoul(argv[1]) : 42;
        if (!seed) throw std::invalid_argument("seed must be nonzero");
        const auto output = std::filesystem::path(argc > 2 ? argv[2] : "build/compactchi-native") / std::to_string(seed);
        std::filesystem::create_directories(output);
        auto config = LoadTestConfig([&](TLLocalConfig& config) {
            config.seed = seed; config.coreCount = 1; config.masterCountPerCoreTLC = 1; config.masterCountPerCoreTLUL = 1;
            config.memoryStart = 0x1000; config.memoryEnd = 0x2000; config.memoryPortCount = 1;
            config.memoryCycleUnit = 1; config.memoryReadLatency = 4; config.memoryWriteLatency = 4;
            config.memoryReadDepth = 16; config.memoryWriteDepth = 8; config.maximumCycle = 10000;
        });
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
        for (unsigned i = 0; i < 8; ++i) { edge(false); edge(true); }
        dut.reset = 0;
        DutAdapter adapter(dut, simulation, output);
        TLSequencer sequencer;
        sequencer.InitializeProtocol(config, adapter);
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
        std::printf("profile=deterministic-read seed=%llu cycles=%llu independent_board=%s trace=%s\n",
                    (unsigned long long)config.seed, (unsigned long long)cycle, passed ? "passed" : "failed", output.c_str());
        return passed && !sequencer.IsFailed() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
