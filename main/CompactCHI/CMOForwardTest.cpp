#include "ForwardNodeBoard.hpp"
#include <cstdio>
#include <fstream>

// Export observations from the unchanged endpoint for the CMO interaction
// fixture. Its CompAck remains a direct host result, never a DUT input.
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected forward observation output path");
        compactchi::ForwardNodeBoard board;
        compactchi::EbData data{1, 12, 501, 4, 53, 8, 0, 2, true};
        data.data.fill(0xa5);
        board.accept(data, 20);
        if (board.tick(21)) throw std::runtime_error("partial DCT completed during CMO");
        data.dataId = 2;
        board.accept(data, 22);
        if (board.tick(22)) throw std::runtime_error("DCT acknowledged inline with CompCMO");
        const auto ack = board.tick(23);
        if (!ack || ack->source != 12 || ack->target != 8 || ack->txn != 53 ||
            ack->opcode != 2 || !board.idle())
            throw std::runtime_error("CMO fixture lost its independent external acknowledgment");
        std::ofstream stream(argv[1]);
        for (const auto& row : board.observations())
            stream << "{\"kind\":\"" << row.kind << "\",\"cycle\":" << row.cycle
                   << ",\"txn\":" << row.txn << ",\"dbid\":" << row.dbid
                   << ",\"source\":" << row.source << ",\"target\":" << row.target
                   << ",\"beat\":" << row.beat << ",\"trace_tag\":" << (row.traceTag ? "true" : "false") << "}\n";
        if (!stream) throw std::runtime_error("cannot write CMO fixture forward observations");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
