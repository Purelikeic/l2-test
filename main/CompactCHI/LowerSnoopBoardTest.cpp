#include "LowerSnoopBoard.hpp"
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
static unsigned mutations = 0;
static void reject(const std::function<void()>& action) {
    bool failed = false;
    try { action(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "lower Snoop checker accepted an invalid observation");
    ++mutations;
}
static compactchi::EbData datum(unsigned target, unsigned txn, unsigned opcode, unsigned dbid,
                                unsigned beat, unsigned resp, unsigned fwd, const compactchi::LineData& bytes) {
    compactchi::EbData data{1, target, txn, opcode, dbid, 8, beat * 2, resp, true};
    data.fwdState = fwd;
    std::copy_n(bytes.begin() + beat * 32, 32, data.data.begin());
    return data;
}
static compactchi::EbResponse response(unsigned txn, unsigned opcode, unsigned resp, unsigned fwd = 0) {
    compactchi::EbResponse result{1, 8, txn, opcode, 0, resp, true};
    result.fwdState = fwd;
    return result;
}
int main() {
    try {
        Context context;
        GlobalBoard<paddr_t> global;
        compactchi::LineData latest{};
        for (unsigned i = 0; i < 64; ++i) latest[i] = uint8_t(i ^ 0xa5);
        auto entry = std::make_shared<Global_SBEntry>();
        entry->status = Global_SBEntry::SB_VALID;
        entry->data = make_shared_tldata<64>();
        std::copy(latest.begin(), latest.end(), entry->data->data);
        global.update(&context, 0x1000, entry);
        compactchi::LowerSnoopBoard board(context, global);

        board.issue({51, 3, 0x1000, false, true}, compactchi::Permission::UD);
        reject([&] { board.retire(51); });
        reject([&] { board.homeData(datum(8, 51, 1, 0, 0, 6, 0, latest)); }); // Once must retain dirty responsibility
        auto wrong = datum(8, 51, 1, 0, 0, 2, 0, latest);
        wrong.data[0] ^= 1;
        reject([&] { board.homeData(wrong); });
        board.homeData(datum(8, 51, 1, 0, 1, 2, 0, latest));
        reject([&] { board.homeData(datum(8, 51, 1, 0, 1, 2, 0, latest)); });
        board.homeData(datum(8, 51, 1, 0, 0, 2, 0, latest));
        const auto once = board.retire(51);
        require(once.dutAfter == compactchi::Permission::UD && once.dirtySink == compactchi::DirtySink::Dut,
                "Once incorrectly transferred dirty ownership");

        board.issue({52, 1, 0x1000, false, true}, compactchi::Permission::UD);
        reject([&] { board.response(response(52, 1, 1)); });
        board.homeData(datum(8, 52, 1, 0, 0, 5, 0, latest));
        board.homeData(datum(8, 52, 1, 0, 1, 5, 0, latest));
        const auto shared = board.retire(52);
        require(shared.dutAfter == compactchi::Permission::SC && shared.dirtySink == compactchi::DirtySink::Home,
                "Shared did not transfer dirty responsibility to Home");

        board.issue({53, 0x17, 0x1000, false, true, 12, 501}, compactchi::Permission::UD);
        reject([&] { board.homeData(datum(8, 53, 6, 0, 0, 4, 6, latest)); });
        auto misrouted = datum(13, 501, 4, 53, 0, 6, 0, latest);
        reject([&] { board.forwardData(misrouted); });
        reject([&] { board.forwardCompAck(12, 8, 53, true); });
        auto stale = datum(12, 501, 4, 53, 0, 6, 0, latest);
        stale.data[0] ^= 1;
        reject([&] { board.forwardData(stale); });
        board.forwardData(datum(12, 501, 4, 53, 1, 6, 0, latest));
        board.forwardData(datum(12, 501, 4, 53, 0, 6, 0, latest));
        reject([&] { board.forwardCompAck(12, 1, 53, true); });
        board.forwardCompAck(12, 8, 53, true);
        require(!board.complete(53), "DCT/CompAck retired a still-stalled Home response");
        reject([&] { board.response(response(53, 9, 0, 2)); });
        board.response(response(53, 9, 0, 6));
        const auto unique = board.retire(53);
        require(unique.dutAfter == compactchi::Permission::I && unique.forwardAfter == compactchi::Permission::UD &&
                unique.dirtySink == compactchi::DirtySink::ForwardNode, "UniqueFwd lost dirty ownership");

        board.issue({54, 0x11, 0x1000, true, true, 12, 502}, compactchi::Permission::UC);
        board.homeData(datum(8, 54, 6, 0, 0, 1, 1, latest));
        board.homeData(datum(8, 54, 6, 0, 1, 1, 1, latest));
        require(!board.complete(54), "Home data retired an incomplete DCT");
        board.forwardData(datum(12, 502, 4, 54, 0, 1, 0, latest));
        board.forwardData(datum(12, 502, 4, 54, 1, 1, 0, latest));
        board.forwardCompAck(12, 8, 54, true);
        const auto cleanShared = board.retire(54);
        require(cleanShared.dutAfter == compactchi::Permission::SC && cleanShared.forwardAfter == compactchi::Permission::SC &&
                cleanShared.dirtySink == compactchi::DirtySink::None, "clean shared forwarding state mismatch");

        board.issue({55, 10, 0x1000, false, true}, compactchi::Permission::UD);
        reject([&] { board.homeData(datum(8, 55, 1, 0, 0, 4, 0, latest)); });
        board.response(response(55, 1, 0));
        require(board.retire(55).dirtySink == compactchi::DirtySink::Discarded, "MakeInvalid returned dirty data");
        board.issue({56, 0x11, 0x1000, false, true, 12, 503}, compactchi::Permission::I);
        board.response(response(56, 1, 0));
        require(board.retire(56).dutAfter == compactchi::Permission::I && board.idle(), "Snoop miss created a DCT obligation");
        std::printf("Lower Snoop/DCT: independent latest data, dirty recipients, Home/DCT stalls and %u mutations passed\n", mutations);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
