#include "EbHome.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>

class Context : public TLLocalContext {
public:
    TLLocalConfig cfg{};
    uint64_t cycles = 0;
    uint64_t cycle() const noexcept override { return cycles; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return cfg.seed; }
    const TLLocalConfig& config() const noexcept override { return cfg; }
    TLLocalConfig& config() noexcept override { return cfg; }
};
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static unsigned rejected = 0;
static void reject(const std::function<void()>& action) {
    bool failed = false;
    try { action(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "Home accepted an invalid protocol/resource observation");
    ++rejected;
}
static void expected(Context& ctx, GlobalBoard<paddr_t>& board, uint64_t address,
                     const chi_agent::Line& data) {
    auto entry = std::make_shared<Global_SBEntry>();
    entry->status = Global_SBEntry::SB_VALID;
    entry->data = make_shared_tldata<64>();
    std::copy(data.begin(), data.end(), entry->data->data);
    board.update(&ctx, address, entry);
}
static compactchi::EbData copy(unsigned dbid, unsigned beat, unsigned resp, bool tag,
                               const chi_agent::Line& data) {
    compactchi::EbData flit{1, 8, dbid, 2, 0, 0, beat * 2, resp, tag};
    std::copy_n(data.begin() + beat * 32, 32, flit.data.begin());
    return flit;
}
int main() {
    try {
        Context ctx;
        ctx.cfg.seed = 42;
        ctx.cfg.memoryStart = 0x1000;
        ctx.cfg.memoryEnd = 0x2000;
        ctx.cfg.memoryReadDepth = 4;
        ctx.cfg.memoryWriteDepth = 1;
        ctx.cfg.memoryReadLatency = 3;
        ctx.cfg.memoryWriteLatency = 2;
        ctx.cfg.memoryCycleUnit = 1;
        ctx.cfg.memoryOOOR = 1;
        GlobalBoard<paddr_t> board;
        chi_agent::HomeMemory memory(ctx.cfg, ctx.cycles);
        compactchi::EbHome home(memory, ctx, board, 8, 4);
        chi_agent::Line initial{};
        for (unsigned i = 0; i < 64; ++i) initial[i] = uint8_t(i ^ 0x85);
        for (uint64_t address : {0x1000U, 0x1040U}) {
            expected(ctx, board, address, initial);
            for (unsigned i = 0; i < 64; ++i) memory.backend().access(address + i) = initial[i];
        }
        auto tick = [&] { memory.tick(); home.service(); ++ctx.cycles; };
        auto waitData = [&] {
            for (unsigned i = 0; i < 80 && !home.dataValid(); ++i) tick();
            require(home.dataValid(), "Home read response did not progress");
        };
        auto drainMemory = [&] {
            for (unsigned i = 0; i < 80 && !home.idle(); ++i) tick();
            require(home.idle(), "Home leaked a terminal memory/protocol obligation");
        };

        compactchi::HomePlan separated;
        separated.separated = true;
        separated.completionAfterData = true;
        separated.upperBeatFirst = true;
        separated.completionSource = 14;
        home.accept({1, 8, 7, 0x26, 0x1000, true}, separated);
        waitData();
        auto held = home.datum();
        require(held.opcode == 0xb && held.dataId == 2 && held.home == 8, "separated read data framing mismatch");
        for (unsigned i = 0; i < 8; ++i) {
            tick();
            require(home.datum().data == held.data && home.datum().txn == held.txn &&
                    home.datum().dbid == held.dbid, "stalled Home DAT changed");
        }
        reject([&] { home.receiveCompAck(1, 14, held.dbid, false); });
        home.dataAccepted();
        home.dataAccepted();
        require(home.responseValid() && home.response().source == 14 && home.response().opcode == 0xb,
                "late separated completion has wrong identity");
        reject([&] { home.receiveCompAck(1, 14, held.dbid, false); });
        home.responseAccepted();
        reject([&] { home.receiveCompAck(1, 8, held.dbid, false); });
        reject([&] { home.receiveCompAck(1, 14, held.dbid, true); });
        home.receiveCompAck(1, 14, held.dbid, false);
        require(home.idle(), "separated read resources did not retire");

        compactchi::HomePlan eviction;
        eviction.responseTraceTag = true;
        // Acquisition precedes its victim generation; their DBIDs remain independent.
        home.accept({1, 8, 9, 7, 0x1000, true});
        home.accept({1, 8, 9, 0x42, 0x1040}, eviction);
        const auto release = home.response();
        home.responseAccepted();
        reject([&] { home.receiveCompAck(1, 8, release.dbid, true); });
        waitData();
        const auto readDbid = home.datum().dbid;
        home.dataAccepted();
        home.dataAccepted();
        reject([&] { home.receiveCompAck(1, 8, readDbid, false); });
        home.receiveCompAck(1, 8, readDbid, true);
        home.receiveCompAck(1, 8, release.dbid, true);
        require(home.idle(), "same-TxnID victim/read resources did not retire independently");

        compactchi::HomePlan clean;
        clean.cleanCopyback = true;
        clean.responseTraceTag = true;
        home.accept({1, 8, 10, 0x42, 0x1040}, clean);
        const auto cleanDbid = home.response().dbid;
        reject([&] { home.receiveCopyback(copy(cleanDbid, 0, 2, true, initial)); });
        home.responseAccepted();
        auto bad = copy(cleanDbid, 0, 2, true, initial);
        bad.data[0] ^= 1;
        reject([&] { home.receiveCopyback(bad); });
        bad = copy(cleanDbid, 0, 2, true, initial);
        bad.byteEnable = 1;
        reject([&] { home.receiveCopyback(bad); });
        home.receiveCopyback(copy(cleanDbid, 0, 2, true, initial));
        reject([&] { home.receiveCopyback(copy(cleanDbid, 0, 2, true, initial)); });
        reject([&] { home.receiveCopyback(copy(cleanDbid, 1, 2, false, initial)); });
        home.receiveCopyback(copy(cleanDbid, 1, 2, true, initial));
        require(!home.requestReady({1, 8, 11, 7, 0x1040}), "read bypassed pending same-line copyback");
        reject([&] { home.receiveCompAck(1, 8, cleanDbid, true); });
        drainMemory();

        auto dirty = initial;
        dirty[0] ^= 0xa5;
        expected(ctx, board, 0x1000, dirty); // independent architectural store
        home.accept({1, 8, 12, 0x1b, 0x1000, false, false});
        const auto dirtyDbid = home.response().dbid;
        home.responseAccepted();
        reject([&] { home.receiveCopyback(copy(dirtyDbid, 0, 2, false, dirty)); });
        reject([&] { home.receiveCopyback(copy(dirtyDbid, 0, 6, false, initial)); });
        home.receiveCopyback(copy(dirtyDbid, 0, 6, false, dirty));
        home.receiveCopyback(copy(dirtyDbid, 1, 6, false, dirty));
        drainMemory();
        for (unsigned i = 0; i < 64; ++i)
            require(memory.backend().access(0x1000 + i) == dirty[i], "dirty writeback did not reach real Home memory");
        reject([&] { home.receiveCompAck(1, 8, dirtyDbid, false); });

        compactchi::HomePlan retryPlan;
        retryPlan.retryOnce = true;
        retryPlan.creditBeforeRetry = true;
        retryPlan.retryIssuer = 14;
        compactchi::EbRequest original{1, 8, 13, 7, 0x1040, true};
        home.accept(original, retryPlan);
        auto reissue = original;
        reissue.allowRetry = false;
        reissue.pCrdType = 3;
        reject([&] { home.accept(reissue); });
        require(home.response().opcode == 7 && home.response().source == 14 && home.response().pCrdType == 3,
                "early P-Credit framing mismatch");
        home.responseAccepted();
        reject([&] { home.accept(reissue); });
        require(home.response().opcode == 3 && home.response().txn == 13, "RetryAck identity mismatch");
        home.responseAccepted();
        auto wrongReissue = reissue;
        wrongReissue.allowRetry = true;
        reject([&] { home.accept(wrongReissue); });
        wrongReissue = reissue;
        wrongReissue.pCrdType = 4;
        reject([&] { home.accept(wrongReissue); });
        wrongReissue = reissue;
        wrongReissue.address = 0x1000;
        reject([&] { home.accept(wrongReissue); });
        wrongReissue = reissue;
        wrongReissue.memAttr = 5;
        reject([&] { home.accept(wrongReissue); });
        wrongReissue = reissue;
        wrongReissue.traceTag = false;
        reject([&] { home.accept(wrongReissue); });
        home.accept(reissue);
        reject([&] { home.accept(reissue); });
        waitData();
        const auto retryDbid = home.datum().dbid;
        home.dataAccepted();
        home.dataAccepted();
        home.receiveCompAck(1, 8, retryDbid, true);
        require(home.idle(), "retry credit or transaction obligation leaked");

        compactchi::HomePlan issuerA;
        issuerA.retryOnce = true;
        compactchi::HomePlan issuerB = issuerA;
        issuerB.retryIssuer = 14;
        compactchi::EbRequest requestA{1, 8, 14, 7, 0x1000};
        compactchi::EbRequest requestB{1, 8, 15, 7, 0x1040};
        home.accept(requestA, issuerA);
        home.accept(requestB, issuerB);
        home.responseAccepted(); // A RetryAck
        home.responseAccepted(); // A credit from issuer 8
        home.responseAccepted(); // B RetryAck from issuer 14
        requestB.allowRetry = false;
        requestB.pCrdType = 3;
        reject([&] { home.accept(requestB); }); // issuer 8 credit cannot pay issuer 14 retry
        home.responseAccepted(); // B credit
        requestA.allowRetry = false;
        requestA.pCrdType = 3;
        home.accept(requestB);
        home.accept(requestA);
        for (unsigned transaction = 0; transaction < 2; ++transaction) {
            waitData();
            const auto dbid = home.datum().dbid;
            home.dataAccepted();
            home.dataAccepted();
            home.receiveCompAck(1, 8, dbid, false);
        }
        require(home.idle(), "two-issuer retry resources did not drain");

        // Same issuer/type credits are fungible. B uses A's earlier credit,
        // completes while its own credit flit is stalled, then A consumes B's
        // still-offered credit after B's transaction context has retired.
        compactchi::EbRequest fungibleA{1, 8, 16, 7, 0x1000};
        compactchi::EbRequest fungibleB{1, 8, 17, 7, 0x1040};
        home.accept(fungibleA, issuerA);
        home.accept(fungibleB, issuerA);
        home.responseAccepted();
        home.responseAccepted();
        home.responseAccepted();
        fungibleB.allowRetry = false;
        fungibleB.pCrdType = 3;
        home.accept(fungibleB);
        waitData();
        const auto fungibleDbid = home.datum().dbid;
        home.dataAccepted();
        home.dataAccepted();
        home.receiveCompAck(1, 8, fungibleDbid, false);
        require(home.responseValid() && home.response().opcode == 7, "stalled credit disappeared with its context");
        home.responseAccepted();
        fungibleA.allowRetry = false;
        fungibleA.pCrdType = 3;
        home.accept(fungibleA);
        waitData();
        const auto finalDbid = home.datum().dbid;
        home.dataAccepted();
        home.dataAccepted();
        home.receiveCompAck(1, 8, finalDbid, false);
        require(home.idle(), "fungible/outliving credit obligations leaked");
        expected(ctx, board, 0x1040, dirty);
        auto wrongSnoop = dirty;
        wrongSnoop[0] ^= 1;
        reject([&] { home.commitSnoopData(0x1040, wrongSnoop); });
        home.commitSnoopData(0x1040, dirty);
        require(!home.idle(), "Home retired dirty Snoop data before memory commitment");
        drainMemory();
        for (unsigned i = 0; i < 64; ++i)
            require(memory.backend().access(0x1040 + i) == dirty[i], "Home Snoop responsibility did not reach memory");
        home.accept({1,8,31,38,0x1000,true});
        reject([&] { home.accept({1,8,31,38,0x1040,false}); });
        waitData();
        const auto oldReadDbid = home.datum().dbid;
        home.dataAccepted(); home.dataAccepted();
        home.accept({1,8,31,38,0x1040,false}); // Old CompAck is still pending.
        home.receiveCompAck(1,8,oldReadDbid,true);
        reject([&] { home.accept({1,8,31,38,0x1000,false}); }); // Old retirement must not remove new owner.
        waitData();
        const auto newReadDbid = home.datum().dbid;
        require(oldReadDbid != newReadDbid,"reused request inherited a live completion DBID");
        home.dataAccepted(); home.dataAccepted();
        home.receiveCompAck(1,8,newReadDbid,false);
        require(home.idle(),"request generations leaked DBID obligations");
        ctx.cfg.mmioEnable = true;
        ctx.cfg.mmioStart = 0x4000; ctx.cfg.mmioEnd = 0x5000;
        home.setDeviceOracle([&](uint64_t) { return std::make_pair(initial, uint64_t(0xff)); });
        compactchi::EbRequest deviceWrite{1,8,2048,0x1c,0x4000,true};
        deviceWrite.size = 3; deviceWrite.allowRetry = true; deviceWrite.expCompAck = false;
        home.accept(deviceWrite); // Default HomePlan tag is false; device must echo request tag.
        require(home.response().traceTag, "MMIO DBIDResp lost original request TraceTag");
        const auto deviceDbid = home.response().dbid;
        home.responseAccepted();
        auto deviceData = copy(deviceDbid, 0, 0, true, initial);
        deviceData.opcode = 3; deviceData.byteEnable = 0xff;
        auto wrongDeviceTag = deviceData; wrongDeviceTag.traceTag = false;
        reject([&] { home.receiveDeviceData(wrongDeviceTag); });
        home.receiveDeviceData(deviceData);
        require(home.idle(), "MMIO write context did not retire");
        auto deviceRead = deviceWrite; deviceRead.opcode = 4; deviceRead.traceTag = false;
        home.accept(deviceRead);
        require(!home.datum().traceTag, "MMIO read data lost original request TraceTag");
        home.dataAccepted();
        require(home.idle(), "MMIO read context did not retire");
        for (const bool early : {false, true}) for (const bool write : {false, true}) {
            auto mmio = deviceWrite;
            mmio.traceTag = false;
            mmio.opcode = write ? 0x1c : 4;
            compactchi::HomePlan retryMmio;
            retryMmio.retryOnce = true;
            retryMmio.creditBeforeRetry = early;
            home.accept(mmio, retryMmio);
            require(home.responseValid() && home.response().opcode == (early ? 7U : 3U),
                    "MMIO first retry/credit response has incorrect order");
            home.responseAccepted();
            require(home.responseValid() && home.response().opcode == (early ? 3U : 7U),
                    "MMIO second retry/credit response has incorrect order");
            home.responseAccepted();
            require(!home.dataValid(), "MMIO data was sent before the credited reissue");
            mmio.allowRetry = false;
            mmio.pCrdType = retryMmio.creditType;
            home.accept(mmio);
            if (write) {
                require(home.response().opcode == 5, "MMIO credited write lacks completion DBID");
                const auto dbid = home.response().dbid;
                home.responseAccepted();
                auto bytes = copy(dbid, 0, 0, false, initial);
                bytes.opcode = 3;
                bytes.byteEnable = 0xff;
                home.receiveDeviceData(bytes);
            } else {
                require(home.dataValid() && home.datum().opcode == 4,
                        "MMIO credited read lacks CompData");
                home.dataAccepted();
            }
            require(home.idle(), "MMIO Retry/P-Credit context failed to drain");
        }
        std::printf("E.b Home: completion/copyback, issuer/type Retry/P-Credit and %u mutations passed\n",
                    rejected);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
