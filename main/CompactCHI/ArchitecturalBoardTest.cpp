#include "ArchitecturalBoard.hpp"
#include <cstdio>
#include <functional>

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
        std::printf("ArchitecturalBoard: legal reads, Compact Snoops, independent data and %u mutation checks passed\n",
                    rejectedMutations);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
