#include "Requester.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>

class Context : public TLLocalContext {
    TLLocalConfig cfg{};
public:
    uint64_t cycle() const noexcept override { return 10; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return 42; }
    const TLLocalConfig& config() const noexcept override { return cfg; }
    TLLocalConfig& config() noexcept override { return cfg; }
};
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void reject(const std::function<void()>& action) {
    bool failed = false;
    try { action(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "requester accepted an invalid transport observation");
}
static compactchi::DataFlit beat(unsigned target, unsigned txn, unsigned dbid, unsigned dataId,
                                unsigned resp, bool traceTag, const compactchi::LineData& data) {
    compactchi::DataFlit result{1, target, txn, dbid, dataId, resp, traceTag};
    std::copy_n(data.begin() + dataId * 32, 32, result.data.begin());
    return result;
}
int main() {
    try {
        Context context;
        compactchi::ArchitecturalBoard board(context, 8);
        compactchi::Requester node(board, 1, 2);
        compactchi::LineData data{};
        for (unsigned i = 0; i < 64; ++i) data[i] = uint8_t(i ^ 0x85);
        board.initialize(0x1000, data);
        board.initialize(0x1040, data);
        node.enqueue({2, 1, 7, 16, 0x1000, true});
        reject([&] { node.enqueue({2, 1, 7, 16, 0x1040, true}); });
        node.requestAccepted();
        auto invalid = beat(2, 7, 77, 0, 2, true, data);
        invalid.data[0] ^= 1;
        reject([&] { node.receiveData(invalid); });
        node.receiveData(beat(2, 7, 77, 1, 2, true, data));
        require(!node.responseValid(), "CompAck preceded complete data");
        node.receiveData(beat(2, 7, 77, 0, 2, true, data));
        require(node.responseValid() && node.response().txn == 77 && node.response().txn != 7 &&
                node.response().traceTag && node.response().opcode == 0,
                "CompAck was not routed by the response DBID");
        require(!node.idle() && !board.idle(), "transaction retired before CompAck handshake");
        reject([&] { node.enqueue({2, 1, 7, 16, 0x1040, true}); });
        node.responseAccepted();
        require(node.idle() && board.idle(), "CompAck did not release requester resources");
        auto dirty = data;
        dirty[0] ^= 0xa5;
        node.store(2, 0x1000, dirty);
        compactchi::ArchitecturalState stableDirty;
        stableDirty.requesters = node.terminalCopies();
        stableDirty.memory = {{0x1000, data}, {0x1040, data}};
        board.checkTerminal(stableDirty); // Dirty residency is allowed with older committed Home bytes.
        node.enqueue({3, 1, 7, 3, 0x1040, false});
        // No REQ handshake occurs while the reverse Snoop is serviced.
        node.receiveSnoop({1, 2, 99, 2, 0x1000, true});
        require(node.requestValid() && node.dataValid() && node.data().opcode == 3 && node.data().resp == 5,
                "Snoop progress depended on the request queue");
        const auto held = node.data();
        reject([&] { node.store(2, 0x1000, data); });
        require(node.data().data == held.data && node.request().address == 0x1040,
                "offered Snoop data or queued request changed under backpressure");
        node.dataAccepted();
        require(board.state(0x1000).permission == compactchi::Permission::UD,
                "node downgraded before complete dirty Snoop return");
        node.dataAccepted();
        require(board.state(0x1000).permission == compactchi::Permission::SC,
                "node did not downgrade after Snoop data completion");
        node.requestAccepted();
        node.receiveData(beat(3, 7, 0, 0, 0, false, data));
        node.receiveData(beat(3, 7, 0, 1, 0, false, data));
        require(!node.responseValid() && node.idle() && board.idle(), "ReadOnce created a persistent ack obligation");
        node.receiveSnoop({1, 2, 100, 0, 0x1000, false});
        require(node.responseValid() && node.response().opcode == 1 && node.response().resp == 0,
                "discard Snoop response was not Invalid");
        node.responseAccepted();
        require(node.idle() && board.idle() && board.state(0x1000).permission == compactchi::Permission::I,
                "discard Snoop left resource ownership");
        auto stable = board.expectedTerminal();
        stable.requesters = node.terminalCopies();
        stable.memory = {{0x1000, data}, {0x1040, data}};
        board.checkTerminal(stable);
        require(stable.requesters.empty(), "Invalid/ReadOnce copies leaked into terminal requester storage");
        {
            compactchi::ArchitecturalBoard pressureBoard(context, 8);
            for (const auto address : {0x1000U,0x1040U,0x1080U}) pressureBoard.initialize(address,data);
            compactchi::Requester pressure(pressureBoard,1,8,2);
            TestAction first{TestActionKind::ReadOnce,0x1000,255,false};
            TestAction second{TestActionKind::ReadOnce,0x1040,0,true};
            require(pressure.ready(2,first),"pressure first request blocked");
            pressure.enqueueAction(2,first); pressure.requestAccepted();
            require(!pressure.nodeIdle(2) && pressure.ready(2,second),"per-node multi-outstanding was serialized");
            auto duplicate=second; duplicate.txn=255;
            require(!pressure.ready(2,duplicate),"live TxnID was reused after wrap");
            auto conflict=second; conflict.address=0x1000;
            require(!pressure.ready(3,conflict),"same-line ownership firewall was lost");
            pressure.enqueueAction(2,second); pressure.requestAccepted();
            TestAction third{TestActionKind::ReadOnce,0x1080,1,false};
            require(pressure.outstanding(2)==2 && !pressure.ready(2,third),"per-node window was not enforced");
            pressure.receiveData(beat(2,0,17,0,0,true,data));
            pressure.receiveData(beat(2,0,17,1,0,true,data));
            require(pressure.ready(2,third) && !pressure.lineIdle(0x1000),"out-of-order release lost another live line");
            pressure.receiveData(beat(2,255,18,0,0,false,data));
            pressure.receiveData(beat(2,255,18,1,0,false,data));
            require(pressure.idle() && pressureBoard.idle(),"pressure completion leaked contexts");
            TestAction acquire{TestActionKind::AcquireUnique,0x1000,1,false};
            pressure.enqueueAction(2,acquire); pressure.requestAccepted();
            pressure.receiveData(beat(2,1,19,0,2,false,data));
            pressure.receiveData(beat(2,1,19,1,2,false,data)); pressure.responseAccepted();
            pressure.store(2,0x1000,data);
            TestAction release{TestActionKind::WriteBack,0x1000,2,false};
            pressure.enqueueAction(2,release);
            require(pressure.snoopReady(),"Snoop was not ready before WBF handshake");
            pressure.eventAccepted();
            require(pressure.snoopReady(),"unrelated WBF globally blocked the pressure Snoop endpoint");
            pressure.receiveSnoop({1,3,77,0,0x1040,false}); // Simultaneous already-sampled Snoop handshake.
            pressure.responseAccepted();
            pressure.receiveResponse({1,2,2,3,0,false},16,0);
            pressure.dataAccepted(); pressure.dataAccepted();
            require(pressure.idle() && pressureBoard.idle(),"simultaneous endpoint handshakes leaked contexts");
        }
        std::puts("Requester: node/TxnID, DBID CompAck, dirty Snoop, independent queues, backpressure and actual terminal copies passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
