#include "ForwardNodeBoard.hpp"
#include <cstdio>
#include <functional>

static void require(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
static void reject(const std::function<void()>& fn) {
    bool failed = false; try { fn(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "ForwardNodeBoard accepted an invalid observation");
}
int main() {
    try {
        compactchi::ForwardNodeBoard board;
        compactchi::EbData first{1, 12, 501, 4, 53, 8, 0, 2, true};
        first.data.fill(0xa5);
        compactchi::EbData second = first; second.dataId = 2; second.data.fill(0x5a);
        reject([&] { auto bad = first; bad.home = 9; board.accept(bad, 10); });
        board.accept(first, 10);
        require(!board.tick(10).has_value(), "forward endpoint acknowledged before both beats");
        reject([&] { board.accept(first, 10); });
        board.accept(second, 10);
        require(!board.tick(10).has_value(), "forward endpoint acknowledged inline");
        auto ack = board.tick(11);
        require(ack && ack->source == 12 && ack->target == 8 && ack->txn == 53 && ack->opcode == 2 && ack->traceTag,
                "forward endpoint generated the wrong delayed CompAck");
        require(board.idle(), "forward endpoint leaked its completed obligation");
        std::puts("ForwardNodeBoard: independent DCT collection, delayed CompAck and mutation checks passed");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
