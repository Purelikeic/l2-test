#include "ArchitecturalBoard.hpp"
#include <cstdio>
#include <functional>
#include <sstream>

class Context : public TLLocalContext {
    TLLocalConfig cfg{};
public:
    uint64_t cycles = 10;
    uint64_t cycle() const noexcept override { return cycles; }
    int sys() const noexcept override { return 0; }
    int sysId() const noexcept override { return 0; }
    unsigned sysSeed() const noexcept override { return 42; }
    const TLLocalConfig& config() const noexcept override { return cfg; }
    TLLocalConfig& config() noexcept override { return cfg; }
};
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static unsigned rejectedMutations = 0;
static void reject(const std::function<void()>& operation) {
    bool failed = false;
    try { operation(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "scoreboard accepted a deliberately invalid observation");
    ++rejectedMutations;
}
int main() {
    try {
        Context context;
        compactchi::ArchitecturalBoard board(context, 8);
        compactchi::LineData initial{};
        for (unsigned i = 0; i < 64; ++i) initial[i] = uint8_t(i ^ 0x85);
        board.initialize(0x1000, initial);
        board.initialize(0x1040, initial);
        reject([&] { board.issue({8, 7}, compactchi::ReadClass::Once, 0x1000, false); });
        board.issue({2, 7}, compactchi::ReadClass::Unique, 0x1000, true);
        reject([&] { board.issue({2, 7}, compactchi::ReadClass::Once, 0x1040, false); });
        reject([&] { board.data({3, 7}, 0, 5, 2, true, initial.data()); });
        reject([&] { board.data({2, 7}, 0, 5, 1, true, initial.data()); });
        reject([&] { board.data({2, 7}, 0, 5, 2, false, initial.data()); });
        auto wrong = initial;
        wrong[31] ^= 1;
        reject([&] { board.data({2, 7}, 0, 5, 2, true, wrong.data()); });
        board.data({2, 7}, 0, 5, 2, true, initial.data());
        reject([&] { board.data({2, 7}, 0, 5, 2, true, initial.data()); });
        reject([&] { board.compAck(2, 5, true); });
        reject([&] { board.data({2, 7}, 1, 6, 2, true, initial.data() + 32); });
        board.data({2, 7}, 1, 5, 2, true, initial.data() + 32);
        reject([&] { board.compAck(3, 5, true); });
        reject([&] { board.compAck(2, 5, false); });
        require(!board.idle(), "scoreboard freed resources before CompAck");
        board.compAck(2, 5, true);
        require(board.idle(), "scoreboard leaked completed read");
        require(board.state(0x1000).permission == compactchi::Permission::UC, "Unique grant state mismatch");
        reject([&] { board.issue({3, 7}, compactchi::ReadClass::Shared, 0x1000, false); });
        auto dirty = initial;
        dirty[0] ^= 0xa5;
        board.store(2, 0x1000, dirty);
        require(board.state(0x1000).permission == compactchi::Permission::UD, "store lost dirty owner");
        board.issue({3, 7}, compactchi::ReadClass::Once, 0x1000, false);
        reject([&] { board.data({3, 7}, 0, 0, 0, false, initial.data()); });
        board.data({3, 7}, 1, 0, 0, false, dirty.data() + 32);
        board.data({3, 7}, 0, 0, 0, false, dirty.data());
        require(board.idle() && board.state(0x1000).owner == 2 &&
                board.state(0x1000).permission == compactchi::Permission::UD,
                "ReadOnce changed persistent dirty ownership");
        board.issue({3, 7}, compactchi::ReadClass::Shared, 0x1040, false);
        board.data({3, 7}, 0, 9, 1, false, initial.data());
        board.data({3, 7}, 1, 9, 1, false, initial.data() + 32);
        board.compAck(3, 9, false);
        reject([&] { board.store(3, 0x1040, dirty); });
        require(board.idle(), "terminal scoreboard resources are not empty");
        board.issue({2, 7}, compactchi::ReadClass::Unique, 0x1000, true);
        board.data({2, 7}, 0, 55, 2, true, initial.data());
        board.data({2, 7}, 1, 55, 2, true, initial.data() + 32);
        board.compAck(2, 55, true);
        require(board.state(0x1000).permission == compactchi::Permission::UD,
                "UD ReadUnique replaced the requester's independently held dirty copy");
        board.issueSnoop({2, 51}, 2, 0x1000, true);
        reject([&] { board.snoopResponse({2, 51}, 1, true); });
        reject([&] { board.snoopData({2, 51}, 0, 1, true, dirty.data()); });
        reject([&] { board.snoopData({2, 51}, 0, 5, false, dirty.data()); });
        reject([&] { board.snoopData({2, 51}, 0, 5, true, initial.data()); });
        board.snoopData({2, 51}, 1, 5, true, dirty.data() + 32);
        require(!board.idle() && board.state(0x1000).permission == compactchi::Permission::UD,
                "Snoop committed before complete dirty data capture");
        reject([&] { board.snoopData({2, 51}, 1, 5, true, dirty.data() + 32); });
        board.snoopData({2, 51}, 0, 5, true, dirty.data());
        require(board.idle() && board.state(0x1000).permission == compactchi::Permission::SC,
                "dirty Shared Snoop did not commit the Shared node state");
        reject([&] { board.store(2, 0x1000, dirty); });
        board.issueSnoop({2, 52}, 3, 0x1000, false);
        reject([&] { board.snoopResponse({2, 52}, 2, false); });
        board.snoopResponse({2, 52}, 1, false);
        board.issueSnoop({2, 53}, 0, 0x1000, false);
        reject([&] { board.snoopData({2, 53}, 0, 0, false, dirty.data()); });
        board.snoopResponse({2, 53}, 0, false);
        board.issueSnoop({2, 54}, 1, 0x1000, false);
        reject([&] { board.snoopData({2, 54}, 0, 0, false, dirty.data()); });
        board.snoopResponse({2, 54}, 0, false);
        require(board.idle() && board.state(0x1000).permission == compactchi::Permission::I,
                "invalidating Snoop leaked node state or obligations");
        // Terminal storage is independent of the transport observations that
        // produced the expected permissions. Clean data may differ from Home.
        auto terminal = board.expectedTerminal();
        require(terminal.requesters.size() == 1 && terminal.requesters.at({3, 0x1040}).permission == compactchi::Permission::SC,
                "terminal model lost another node's clean residency");
        require(terminal.memory.at(0x1000) == initial, "uncommitted store changed expected Home memory");
        board.checkTerminal(terminal);
        auto corrupt = terminal;
        corrupt.requesters.erase({3, 0x1040}); reject([&] { board.checkTerminal(corrupt); });
        corrupt = terminal; corrupt.requesters.at({3, 0x1040}).permission = compactchi::Permission::UC;
        reject([&] { board.checkTerminal(corrupt); });
        corrupt = terminal; corrupt.requesters.at({3, 0x1040}).data[63] ^= 1;
        reject([&] { board.checkTerminal(corrupt); });
        corrupt = terminal; corrupt.requesters.emplace(std::make_pair(4U, uint64_t(0x1040)), compactchi::ResidentLine{compactchi::Permission::SC, initial});
        reject([&] { board.checkTerminal(corrupt); });
        corrupt = terminal; corrupt.memory.at(0x1000) = dirty;
        reject([&] { board.checkTerminal(corrupt); });
        corrupt = terminal; corrupt.memory.erase(0x1040);
        reject([&] { board.checkTerminal(corrupt); });
        board.issue({4, 8}, compactchi::ReadClass::Once, 0x1040, false);
        reject([&] { board.checkTerminal(terminal); });
        board.data({4, 8}, 0, 0, 0, false, initial.data());
        board.data({4, 8}, 1, 0, 0, false, initial.data() + 32);
        board.checkTerminal(terminal); // ReadOnce creates no terminal resident copy.
        TestAction device{TestActionKind::WriteNoSnp, 0x2008, 9, false, 3};
        device.byteEnable = uint64_t(0xff) << 8; device.data.fill(0xa7);
        board.issueControl({7, 9}, device); board.controlDBID({7, 9}, 1, false);
        board.controlSent({7, 9}, 0); board.controlComplete({7, 9}, 1, 0, false);
        terminal.devices[0x2000] = {};
        std::fill_n(terminal.devices.at(0x2000).begin() + 8, 8, 0xa7);
        board.checkTerminal(terminal);
        corrupt = terminal; corrupt.devices.at(0x2000)[63] = 1;
        reject([&] { board.checkTerminal(corrupt); }); // Untouched bytes matter too.
        corrupt = terminal; corrupt.devices.clear(); reject([&] { board.checkTerminal(corrupt); });
        board.memoryCommitted(0x1000, dirty);
        reject([&] { board.checkTerminal(terminal); });
        terminal.memory[0x1000] = dirty;
        std::ostringstream evidence; board.checkTerminal(terminal, &evidence);
        require(evidence.str().find("\"observed\"") != std::string::npos, "terminal evidence missing actual storage");
        // Exercise the online board's real CMO path. Probe an Invalid identity
        // so final requester state alone cannot detect a still-live Snoop.
        for (const auto kind : {TestActionKind::Clean, TestActionKind::Flush, TestActionKind::Invalidate}) {
            compactchi::ArchitecturalBoard cmoBoard(context, 8);
            cmoBoard.initialize(0x3000, initial);
            TestAction cmo{kind, 0x3000, 1, true};
            cmoBoard.issueControl({0, 1}, cmo);
            cmoBoard.issueSnoop({1, 2}, 1, 0x3000, true);
            reject([&] { cmoBoard.controlComplete({0, 1}, 4, 0, true); });
            cmoBoard.snoopResponse({1, 2}, 0, true);
            cmoBoard.controlComplete({0, 1}, 4, 0, true);
            require(cmoBoard.idle(), "completed CMO retained a Snoop/control owner");
        }
        // A clean valid snoopee may return two data beats. Its already legal
        // state must not permit CompCMO to retire after just the first beat.
        compactchi::ArchitecturalBoard partialBoard(context, 8);
        partialBoard.initialize(0x4000, initial);
        partialBoard.issue({0, 3}, compactchi::ReadClass::Shared, 0x4000, false);
        partialBoard.data({0, 3}, 0, 4, 1, false, initial.data());
        partialBoard.data({0, 3}, 1, 4, 1, false, initial.data() + 32);
        partialBoard.compAck(0, 4, false);
        partialBoard.issueControl({0, 5}, {TestActionKind::Clean, 0x4000, 5, true});
        partialBoard.issueSnoop({0, 6}, 2, 0x4000, true);
        partialBoard.snoopData({0, 6}, 1, 1, true, initial.data() + 32);
        reject([&] { partialBoard.controlComplete({0, 5}, 4, 0, true); });
        partialBoard.snoopData({0, 6}, 0, 1, true, initial.data());
        partialBoard.controlComplete({0, 5}, 4, 0, true);
        require(partialBoard.idle(), "two-beat CMO Snoop failed to retire");
        std::printf("ArchitecturalBoard: legal reads, Compact Snoops, online CMO Snoop retirement, terminal state and %u mutation checks passed\n",
                    rejectedMutations);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
