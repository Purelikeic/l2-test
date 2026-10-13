#pragma once
#include "../Fuzzer/NodeFuzzer.hpp"
#include <algorithm>
#include <istream>
#include <limits>
#include <sstream>
#include <string>

namespace compactchi {
inline unsigned replayNibble(char c) {
    if (c >= '0' && c <= '9') return unsigned(c-'0');
    if (c >= 'a' && c <= 'f') return unsigned(c-'a'+10);
    if (c >= 'A' && c <= 'F') return unsigned(c-'A'+10);
    throw std::invalid_argument("action replay has non-hex data");
}
inline uint64_t replayInteger(const std::string& text) {
    if (text.empty() || !std::all_of(text.begin(),text.end(),[](char c) { return c >= '0' && c <= '9'; }))
        throw std::invalid_argument("action replay requires unsigned decimal fields");
    return std::stoull(text);
}
inline std::vector<PlannedTestAction> readActionReplay(std::istream& input) {
    std::string line;
    if (!std::getline(input,line) || line != "ANSERL2_ACTIONS_V1")
        throw std::invalid_argument("action replay version/header mismatch");
    std::vector<PlannedTestAction> result;
    while (std::getline(input,line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        std::array<std::string,8> field;
        for (auto& f:field) if (!(row >> f)) throw std::invalid_argument("incomplete action replay row");
        std::string extra;
        if (row >> extra) throw std::invalid_argument("extra action replay fields");
        const auto node=replayInteger(field[0]), kind=replayInteger(field[1]);
        const auto address=replayInteger(field[2]), txn=replayInteger(field[3]), tag=replayInteger(field[4]);
        const auto size=replayInteger(field[5]), mask=replayInteger(field[6]);
        if (node > std::numeric_limits<unsigned>::max() || kind > 11 || txn > 255 || tag > 1 || size > 6 ||
            (address & ((uint64_t(1) << size)-1)) || field[7].size() != 128)
            throw std::invalid_argument("action replay identity/size/data range invalid");
        TestAction action{TestActionKind(kind),address,unsigned(txn),tag != 0};
        action.size=unsigned(size); action.byteEnable=mask;
        for (unsigned i=0;i<64;++i) action.data[i]=uint8_t((replayNibble(field[7][2*i]) << 4) | replayNibble(field[7][2*i+1]));
        result.push_back({unsigned(node),action});
        if (result.size() > 256) throw std::invalid_argument("action replay exceeds 256 actions");
    }
    if (!input.eof() || result.empty()) throw std::invalid_argument("empty/unreadable action replay");
    return result;
}
} // namespace compactchi
