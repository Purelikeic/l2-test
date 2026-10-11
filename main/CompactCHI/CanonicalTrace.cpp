#include "CanonicalTrace.hpp"
#include <stdexcept>

namespace compactchi {
namespace {
const char* name(Channel channel) {
    static constexpr const char* names[] = {
        "cchi_rx_req", "cchi_rx_evt", "cchi_rx_rsp", "cchi_rx_dat", "cchi_tx_snp", "cchi_tx_rsp", "cchi_tx_dat",
        "chi_tx_req", "chi_rx_rsp", "chi_rx_dat", "chi_rx_snp", "chi_tx_rsp", "chi_tx_dat"
    };
    const auto index = unsigned(channel);
    if (index >= sizeof(names) / sizeof(names[0])) throw std::invalid_argument("unknown canonical channel");
    return names[index];
}
template<class T> void field(std::ostream& stream, const char* key, const std::optional<T>& value) {
    stream << ",\"" << key << "\":";
    if (value) stream << *value; else stream << "null";
}
void flag(std::ostream& stream, const char* key, const std::optional<bool>& value) {
    stream << ",\"" << key << "\":";
    if (value) stream << (*value ? "true" : "false"); else stream << "null";
}
}
CanonicalTrace::CanonicalTrace(const std::filesystem::path& prefix, unsigned seed) : seed(seed) {
    if (prefix.has_parent_path()) std::filesystem::create_directories(prefix.parent_path());
    jsonl.open(prefix.string() + ".jsonl");
    text.open(prefix.string() + ".log");
    if (!jsonl || !text) throw std::runtime_error("cannot open canonical trace output");
    jsonl << "{\"schema\":1,\"kind\":\"run\",\"seed\":" << seed << "}\n";
    text << "seed=" << seed << " schema=1\n";
}
void CanonicalTrace::record(const CanonicalEvent& event) {
    if (lastCycle && event.cycle < *lastCycle) throw std::invalid_argument("canonical cycle moved backwards");
    const auto direction = name(event.channel);
    lastCycle = event.cycle;
    jsonl << "{\"schema\":1,\"kind\":\"event\",\"seed\":" << seed
          << ",\"cycle\":" << event.cycle << ",\"direction\":\"" << direction
          << "\",\"node\":" << event.node << ",\"target\":" << event.target
          << ",\"txn_id\":" << event.txn << ",\"opcode\":" << event.opcode
          << ",\"valid\":" << (event.valid ? "true" : "false")
          << ",\"ready\":" << (event.ready ? "true" : "false")
          << ",\"handshake\":" << (event.valid && event.ready ? "true" : "false")
          << ",\"trace_tag\":" << (event.traceTag ? "true" : "false");
    field(jsonl, "sim_time", event.simTime);
    field(jsonl, "address", event.address);
    field(jsonl, "dbid", event.dbid);
    field(jsonl, "data_id", event.dataId);
    field(jsonl, "resp", event.resp);
    field(jsonl, "resp_err", event.respErr);
    field(jsonl, "state_before", event.beforeState);
    field(jsonl, "state_after", event.afterState);
    field(jsonl, "owner", event.owner);
    field(jsonl, "data_source", event.dataSource);
    field(jsonl, "prefetch_source", event.prefetchSource);
    field(jsonl, "forward_state", event.forwardState);
    field(jsonl, "home_node", event.homeNode);
    field(jsonl, "forward_node", event.forwardNode);
    field(jsonl, "forward_txn", event.forwardTxn);
    field(jsonl, "byte_enable", event.byteEnable);
    flag(jsonl, "allow_retry", event.allowRetry);
    flag(jsonl, "exp_comp_ack", event.expCompAck);
    flag(jsonl, "ns", event.ns);
    flag(jsonl, "likely_shared", event.likelyShared);
    field(jsonl, "p_crd_type", event.pCrdType);
    field(jsonl, "size", event.size);
    field(jsonl, "mem_attr", event.memAttr);
    field(jsonl, "order", event.order);
    jsonl << ",\"dirty\":";
    if (event.dirty) jsonl << (*event.dirty ? "true" : "false"); else jsonl << "null";
    jsonl << ",\"data\":";
    if (event.data) {
        static constexpr char digits[] = "0123456789abcdef";
        jsonl << '"';
        for (const auto value : *event.data) jsonl << digits[value >> 4] << digits[value & 15];
        jsonl << '"';
    } else jsonl << "null";
    jsonl << "}\n";
    text << "cycle=" << event.cycle << " direction=" << direction << " node=" << event.node
         << " txn=" << event.txn << " opcode=" << event.opcode
         << " handshake=" << (event.valid && event.ready) << " TraceTag=" << event.traceTag;
    if (event.simTime) text << " sim_time=" << *event.simTime;
    if (event.address) text << " address=" << *event.address;
    if (event.dbid) text << " dbid=" << *event.dbid;
    text << '\n';
    // Keep the last handshake available if a DUT assertion aborts the process.
    jsonl.flush();
    text.flush();
    if (!jsonl || !text) throw std::runtime_error("canonical trace write failed");
}
} // namespace compactchi
