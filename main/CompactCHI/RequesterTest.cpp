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
        std::puts("Requester: node/TxnID, DBID CompAck, dirty Snoop, independent queues and backpressure passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
