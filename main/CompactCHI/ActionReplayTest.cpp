#include "ActionReplay.hpp"
#include <sstream>
#include <cstdio>

static void require(bool ok) { if (!ok) throw std::runtime_error("action replay format check failed"); }
static void reject(const std::string& text) {
    bool failed=false;
    try { std::istringstream input(text); compactchi::readActionReplay(input); }
    catch (const std::exception&) { failed=true; }
    require(failed);
}
int main() {
    const std::string data(128,'a');
    const std::string header="ANSERL2_ACTIONS_V1\n";
    std::istringstream input(header+"0 11 4096 37 1 6 18446744073709551615 "+data+"\n"+
        "4 10 262144 38 0 3 165 "+data+"\n");
    const auto actions=compactchi::readActionReplay(input);
    require(actions.size()==2 && actions[0].node==0 && actions[0].action.kind==TestActionKind::Store);
    require(actions[0].action.address==4096 && actions[0].action.txn==37 && actions[0].action.traceTag);
    require(actions[0].action.byteEnable==~uint64_t(0) && actions[0].action.data[0]==0xaa && actions[0].action.data[63]==0xaa);
    require(actions[1].action.kind==TestActionKind::WriteNoSnp && actions[1].action.size==3 && actions[1].action.byteEnable==165);
    reject("ANSERL2_ACTIONS_V2\n"); reject(header); reject(header+"0 11 4096\n");
    for (const auto& prefix:{"0 12 4096 1 0 6 0 ","0 11 4096 256 0 6 0 ",
        "0 11 4096 1 2 6 0 ","0 11 4096 1 0 7 0 ","0 11 4097 1 0 6 0 ","-1 11 4096 1 0 6 0 "})
        reject(header+prefix+data+"\n");
    reject(header+"0 11 4096 1 0 6 0 "+std::string(127,'a')+"\n");
    reject(header+"0 11 4096 1 0 6 0 "+std::string(128,'x')+"\n");
    reject(header+"0 11 4096 1 0 6 0 "+data+" extra\n");
    std::puts("Action replay format: full store/MMIO intent and 12 malformed/version/bounds negatives passed");
}
