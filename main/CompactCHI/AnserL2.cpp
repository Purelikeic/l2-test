#include "VL2Cache.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Beat = std::array<uint32_t, 8>;
struct Dat { unsigned txn, beat, opcode, resp, dbid; uint64_t line; };
struct Rsp { unsigned txn, opcode, dbid, kind; unsigned source = 8; };
struct UpRsp { unsigned txn, opcode, resp; };
struct Snp { unsigned txn; uint64_t line; unsigned opcode; };
static Beat dataFor(uint64_t line, unsigned beat) {
  Beat data;
  for (unsigned i = 0; i < 8; ++i) data[i] = uint32_t((line * 0x9e3779b9ULL) ^ (beat * 0x7f4a7c15ULL) ^ (i * 0x85ebca6bULL));
  return data;
}
static void poke(VlWide<8>& port, const Beat& data) { for (unsigned i = 0; i < 8; ++i) port[i] = data[i]; }
static void check(const VlWide<8>& port, const Beat& data, const std::string& msg) {
  for (unsigned i = 0; i < 8; ++i) if (port[i] != data[i]) throw std::runtime_error(msg + " word=" + std::to_string(i) + " got=" + std::to_string(port[i]) + " expected=" + std::to_string(data[i]));
}
int main(int argc, char** argv) {
  VerilatedContext context;
  context.commandArgs(argc, argv);
  VL2Cache dut{&context};
  uint32_t rng = argc > 1 ? std::stoul(argv[1]) : 42;
  auto random = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
  unsigned cycle = 0, accepted = 0, completed = 0, downReads = 0, writebacks = 0, snoops = 0, retries = 0;
  unsigned nextDbid = 128, nextSnp = 2048;
  std::deque<Dat> dat;
  std::deque<Rsp> rsp;
  std::map<unsigned, Rsp> lateCompletion;
  std::map<unsigned, unsigned> receivedDataMask;
  std::deque<UpRsp> upRsp;
  std::deque<Snp> snp;
  std::map<unsigned, uint64_t> address, writes, snoopAddress;
  std::map<unsigned, unsigned> opcodes, beatMask, responseDbid, writeMask, snpMask;
  std::set<unsigned> completedTxns, retried;
  std::map<std::string, std::vector<uint64_t>> held;
  bool reqActive = false;
  unsigned reqOp = 0, reqTxn = 0, reqAlias = 0;
  uint64_t reqLine = 0;
  unsigned quiet = 0, flushPhase = 0, flushCycles = 0, wbfPhase = 0;
  const uint64_t modifiedLine = (0x100000ULL >> 6) + 12 * 512;
  bool modified = false;
  std::deque<Dat> wbfData;
  std::deque<Dat> snoopData;
  bool clientDirty = false;
  unsigned version = 0, dirtySnoopBeats = 0, dctBeats = 0;
  std::map<unsigned, unsigned> expectedCompAck;
  auto expectedData = [&](uint64_t line, unsigned beat) {
    auto value = dataFor(line, beat);
    if (modified && line == modifiedLine) {
      value[0] ^= version == 1 ? 0xa55aa55aU : 0x12345678U;
      value[7] ^= version == 1 ? 0x5aa55aa5U : 0x87654321U;
    }
    return value;
  };
  bool finished = false;
  auto tick = [&]() { dut.clock = 1; dut.eval(); context.timeInc(1); dut.clock = 0; dut.eval(); context.timeInc(1); ++cycle; };
  auto stable = [&](const std::string& name, bool valid, bool ready, std::vector<uint64_t> fields) {
    if (held.count(name) && (!valid || held.at(name) != fields)) throw std::runtime_error("unstable " + name);
    if (valid && !ready) held[name] = fields; else held.erase(name);
  };
  auto zeroInputs = [&]() {
    dut.io_req_valid = 0; dut.io_evt_valid = 0; dut.io_prefetchTask_valid = 0;
    dut.io_downDat_valid = 0; dut.io_downRsp_valid = 0; dut.io_downSnp_valid = 0;
    dut.io_upRspIn_valid = 0; dut.io_upDatIn_valid = 0;
    dut.io_upRsp_ready = 1; dut.io_upDat_ready = 1; dut.io_upSnp_ready = 1;
    dut.io_downReq_ready = 1; dut.io_downTxRsp_ready = 1; dut.io_downTxDat_ready = 1;
    dut.io_l2NodeId = 1; dut.io_homeNodeId = 8; dut.io_vaddr = 0; dut.io_flush = 0;
  };
  zeroInputs(); dut.reset = 1;
  for (unsigned i = 0; i < 8; ++i) tick();
  dut.reset = 0;
  try {
    while (cycle < 30000 && !finished) {
      zeroInputs();
      dut.io_flush = flushPhase == 1 || flushPhase == 3;
      // Bursts of long output stalls are combined with independent randomized readiness.
      const bool stall = cycle % 401 >= 130 && cycle % 401 < 180;
      dut.io_upDat_ready = !stall && random() % 4 != 0;
      dut.io_upRsp_ready = !stall && random() % 4 != 0;
      dut.io_upSnp_ready = random() % 4 != 0;
      dut.io_downReq_ready = random() % 4 != 0;
      dut.io_downTxRsp_ready = !stall && random() % 4 != 0;
      dut.io_downTxDat_ready = !stall && random() % 4 != 0;
      if (!reqActive && accepted < 131 && (accepted < 128 || completed == accepted)) {
        reqActive = true; reqTxn = accepted + 1;
        const unsigned selector = random() % 3;
        reqOp = selector == 0 ? 3 : selector == 1 ? 4 : 16;
        reqLine = (0x100000ULL >> 6) + (random() % 7) * 512 + (random() % 4);
        reqAlias = random() & 1;
        if (reqTxn == 129) { reqOp = 18; reqLine = modifiedLine; }
        if (reqTxn == 130) { reqOp = 3; reqLine = modifiedLine; }
        if (reqTxn == 131) { reqOp = 16; reqLine = modifiedLine; }
      }
      if (reqActive) {
        dut.io_req_valid = 1; dut.io_req_bits_opcode = reqOp;
        dut.io_req_bits_txnID = reqTxn; dut.io_req_bits_addr = reqLine << 6;
        dut.io_req_bits_size = 6; dut.io_req_bits_srcID = 2; dut.io_req_bits_tgtID = 1;
        dut.io_req_bits_tagAlias = reqAlias; dut.io_req_bits_ns = 0; dut.io_req_bits_order = 0;
        dut.io_req_bits_memAttr = 0; dut.io_req_bits_excl = 0; dut.io_req_bits_expCompData = 1;
        dut.io_req_bits_expCompStash = 0; dut.io_req_bits_wayValid = 0;
        dut.io_req_bits_way = 0; dut.io_req_bits_traceTag = 0;
      }
      if (completed == 131 && wbfPhase == 0) {
        dut.io_evt_valid = 1; dut.io_evt_bits_txnID = 200;
        dut.io_evt_bits_srcID = 2; dut.io_evt_bits_tgtID = 1;
        dut.io_evt_bits_opcode = 1; dut.io_evt_bits_addr = modifiedLine << 6;
        dut.io_evt_bits_ns = 0; dut.io_evt_bits_memAttr = 0;
        dut.io_evt_bits_wayValid = 0; dut.io_evt_bits_way = 0; dut.io_evt_bits_traceTag = 0;
      }
      const bool sendingSnoopData = !snoopData.empty();
      if (sendingSnoopData || !wbfData.empty()) {
        const auto& x = sendingSnoopData ? snoopData.front() : wbfData.front();
        dut.io_upDatIn_valid = 1; dut.io_upDatIn_bits_txnID = x.txn;
        dut.io_upDatIn_bits_srcID = 2; dut.io_upDatIn_bits_tgtID = 1;
        dut.io_upDatIn_bits_opcode = x.opcode; dut.io_upDatIn_bits_resp = x.resp;
        dut.io_upDatIn_bits_respErr = 0; dut.io_upDatIn_bits_dataID = x.beat;
        dut.io_upDatIn_bits_byteEnable = 0xffffffffU; dut.io_upDatIn_bits_traceTag = 0;
        poke(dut.io_upDatIn_bits_data, expectedData(x.line, x.beat));
      }
      if (!dat.empty()) {
        const auto& x = dat.front();
        dut.io_downDat_valid = 1; dut.io_downDat_bits_txnID = x.txn;
        dut.io_downDat_bits_srcID = 8; dut.io_downDat_bits_tgtID = 1;
        dut.io_downDat_bits_opcode = x.opcode; dut.io_downDat_bits_dbID = x.dbid;
        dut.io_downDat_bits_homeNID = 8; dut.io_downDat_bits_respErr = 0;
        dut.io_downDat_bits_resp = x.resp; dut.io_downDat_bits_dataID = x.beat * 2;
        poke(dut.io_downDat_bits_data, expectedData(x.line, x.beat));
      }
      if (!rsp.empty()) {
        const auto& x = rsp.front();
        dut.io_downRsp_valid = 1; dut.io_downRsp_bits_txnID = x.txn;
        dut.io_downRsp_bits_srcID = x.source; dut.io_downRsp_bits_tgtID = 1;
        dut.io_downRsp_bits_opcode = x.opcode; dut.io_downRsp_bits_dbID = x.dbid;
        dut.io_downRsp_bits_resp = 2; dut.io_downRsp_bits_respErr = 0;
        dut.io_downRsp_bits_pCrdType = x.kind; dut.io_downRsp_bits_cBusy = 0;
      }
      if (!upRsp.empty()) {
        const auto& x = upRsp.front();
        dut.io_upRspIn_valid = 1; dut.io_upRspIn_bits_txnID = x.txn;
        dut.io_upRspIn_bits_srcID = 2; dut.io_upRspIn_bits_tgtID = 1;
        dut.io_upRspIn_bits_opcode = x.opcode; dut.io_upRspIn_bits_resp = x.resp;
        dut.io_upRspIn_bits_respErr = 0; dut.io_upRspIn_bits_traceTag = 0;
      }
      if (!snp.empty()) {
        const auto& x = snp.front();
        dut.io_downSnp_valid = 1; dut.io_downSnp_bits_txnID = x.txn;
        dut.io_downSnp_bits_addr = x.line << 3; dut.io_downSnp_bits_srcID = 8;
        const bool forwarded = x.opcode == 0x11 || x.opcode == 0x17;
        dut.io_downSnp_bits_opcode = x.opcode; dut.io_downSnp_bits_fwdNID = forwarded ? 12 : 0;
        dut.io_downSnp_bits_fwdTxnID = forwarded ? x.txn + 512 : 0;
        dut.io_downSnp_bits_retToSrc = x.opcode != 9 && x.opcode != 0xa && x.opcode != 0x17;
        dut.io_downSnp_bits_ns = 0; dut.io_downSnp_bits_traceTag = 0;
      }
      dut.eval();
      stable("REQ", dut.io_downReq_valid, dut.io_downReq_ready,
        {dut.io_downReq_bits_txnID, dut.io_downReq_bits_opcode, dut.io_downReq_bits_addr,
         dut.io_downReq_bits_allowRetry, dut.io_downReq_bits_pCrdType});
      std::vector<uint64_t> upFields = {dut.io_upDat_bits_txnID, dut.io_upDat_bits_dbID,
        dut.io_upDat_bits_dataID, dut.io_upDat_bits_respErr, dut.io_upDat_bits_resp};
      for (unsigned i=0;i<8;++i) upFields.push_back(dut.io_upDat_bits_data[i]);
      stable("upDAT", dut.io_upDat_valid, dut.io_upDat_ready, upFields);
      std::vector<uint64_t> downFields = {dut.io_downTxDat_bits_txnID, dut.io_downTxDat_bits_opcode,
        dut.io_downTxDat_bits_dataID, dut.io_downTxDat_bits_resp, dut.io_downTxDat_bits_byteEnable};
      for (unsigned i=0;i<8;++i) downFields.push_back(dut.io_downTxDat_bits_data[i]);
      stable("downDAT", dut.io_downTxDat_valid, dut.io_downTxDat_ready, downFields);
      stable("upRSP", dut.io_upRsp_valid, dut.io_upRsp_ready,
        {dut.io_upRsp_bits_txnID, dut.io_upRsp_bits_opcode, dut.io_upRsp_bits_dbID, dut.io_upRsp_bits_respErr, dut.io_upRsp_bits_resp});
      stable("upSNP", dut.io_upSnp_valid, dut.io_upSnp_ready,
        {dut.io_upSnp_bits_txnID, dut.io_upSnp_bits_opcode, dut.io_upSnp_bits_addr});
      stable("downRSP", dut.io_downTxRsp_valid, dut.io_downTxRsp_ready,
        {dut.io_downTxRsp_bits_txnID, dut.io_downTxRsp_bits_opcode, dut.io_downTxRsp_bits_resp});
      // Retire input fronts before adding newly generated responses.
      if (dut.io_downDat_valid && dut.io_downDat_ready) {
        const auto x = dat.front();
        receivedDataMask[x.dbid] |= 1U << x.beat;
        if (receivedDataMask[x.dbid] == 3 && lateCompletion.count(x.dbid)) {
          rsp.push_back(lateCompletion.at(x.dbid));
          lateCompletion.erase(x.dbid);
        }
        dat.pop_front();
      }
      if (dut.io_downRsp_valid && dut.io_downRsp_ready) rsp.pop_front();
      if (dut.io_upRspIn_valid && dut.io_upRspIn_ready) upRsp.pop_front();
      if (dut.io_evt_valid && dut.io_evt_ready) { modified = true; version = 2; wbfPhase = 1; }
      if (dut.io_upRsp_valid && dut.io_upRsp_ready) {
        if (dut.io_upRsp_bits_txnID == 129 && dut.io_upRsp_bits_opcode == 1) {
          if (!address.count(129) || completedTxns.count(129) || dut.io_upRsp_bits_resp != 2)
            throw std::runtime_error("invalid MakeUnique completion");
          completedTxns.insert(129); ++completed;
          modified = true; version = 1; clientDirty = true; // requester completes the full-line overwrite
          upRsp.push_back({dut.io_upRsp_bits_dbID, 0, 0});
        } else {
          if (wbfPhase != 1 || dut.io_upRsp_bits_txnID != 200 || dut.io_upRsp_bits_opcode != 3)
            throw std::runtime_error("unknown WriteBackFull response");
          wbfPhase = 2;
          wbfData.push_back({dut.io_upRsp_bits_dbID, 0, 2, 6, 0, modifiedLine});
          wbfData.push_back({dut.io_upRsp_bits_dbID, 1, 2, 6, 0, modifiedLine});
        }
      }
      if (dut.io_upDatIn_valid && dut.io_upDatIn_ready) {
        if (sendingSnoopData) {
          snoopData.pop_front(); ++dirtySnoopBeats;
        } else {
          wbfData.pop_front();
          if (wbfData.empty()) {
            wbfPhase = 3;
            const unsigned txn = nextSnp++; snoopAddress[txn] = modifiedLine;
            snp.push_back({txn, modifiedLine, 0x11});
            snoopAddress[txn + 512] = modifiedLine;
          }
        }
      }
      if (dut.io_downSnp_valid && dut.io_downSnp_ready) { ++snoops; snp.pop_front(); }
      if (dut.io_req_valid && dut.io_req_ready) {
        address[reqTxn] = reqLine; opcodes[reqTxn] = reqOp; ++accepted; reqActive = false;
      }
      if (dut.io_downReq_valid && dut.io_downReq_ready) {
        const unsigned txn = dut.io_downReq_bits_txnID, op = dut.io_downReq_bits_opcode;
        if ((dut.io_downReq_bits_addr & 63) || dut.io_downReq_bits_size != 6 ||
            !(dut.io_downReq_bits_memAttr & 4)) throw std::runtime_error("invalid cacheable line framing");
        if (dut.io_downReq_bits_allowRetry && dut.io_downReq_bits_pCrdType != 0)
          throw std::runtime_error("initial request used retry credit");
        const uint64_t line = dut.io_downReq_bits_addr >> 6;
        if ((op == 0x26 || op == 7) && dut.io_downReq_bits_allowRetry && txn % 7 == 3 && !retried.count(txn)) {
          retried.insert(txn); ++retries;
          // Grant precedes RetryAck to exercise early credit ownership.
          rsp.push_back({0, 7, 0, 3}); rsp.push_back({txn, 3, 0, 3});
        } else if (op == 0x26 || op == 7) {
          ++downReads;
          const unsigned dbid = nextDbid++;
          const bool split = downReads % 3 == 0;
          expectedCompAck[dbid] = split ? 14 : 8;
          const unsigned response = op == 7 ? (downReads % 4 == 0 ? 6 : 2) : 1;
          if (split) {
            const Rsp completion{txn, 0xb, dbid, 0, 14};
            if (downReads % 2) lateCompletion[dbid] = completion;
            else rsp.push_back(completion);
          }
          const unsigned firstBeat = downReads % 2;
          dat.push_back({txn, firstBeat, split ? 0xbU : 4U, response, dbid, line});
          dat.push_back({txn, 1 - firstBeat, split ? 0xbU : 4U, response, dbid, line});
        } else if (op == 0xd || op == 0x10 || op == 0x11 || op == 0xf || op == 0xc) {
          const unsigned dbid = nextDbid++;
          rsp.push_back({txn, 4, dbid, 0});
          if (op == 0xc) expectedCompAck[dbid] = 8;
        } else if (op == 0x42) {
          const unsigned dbid = nextDbid++;
          if (downReads % 2 == 0) { rsp.push_back({txn, 4, dbid, 0}); expectedCompAck[dbid] = 8; }
          else { writes[dbid] = line; rsp.push_back({txn, 5, dbid, 0}); ++writebacks; }
        } else if (op == 0x1b || op == 0x17) {
          const unsigned dbid = nextDbid++; writes[dbid] = line;
          rsp.push_back({txn, 5, dbid, 0}); ++writebacks;
        } else throw std::runtime_error("unsupported downREQ " + std::to_string(op));
      }
      if (dut.io_upDat_valid && dut.io_upDat_ready) {
        const unsigned txn = dut.io_upDat_bits_txnID, beat = dut.io_upDat_bits_dataID;
        if (!address.count(txn) || completedTxns.count(txn) || (beatMask[txn] & (1U << beat)))
          throw std::runtime_error("duplicate/unknown upDAT " + std::to_string(txn));
        check(dut.io_upDat_bits_data, expectedData(address[txn], beat), "upDAT txn=" + std::to_string(txn));
        if (responseDbid.count(txn) && responseDbid[txn] != dut.io_upDat_bits_dbID)
          throw std::runtime_error("changed DBID");
        responseDbid[txn] = dut.io_upDat_bits_dbID; beatMask[txn] |= 1U << beat;
        if (opcodes[txn] == 16 && (dut.io_upDat_bits_resp & 3) != 2) throw std::runtime_error("missing Unique permission");
        if (beatMask[txn] == 3) {
          completedTxns.insert(txn); ++completed;
          if (opcodes[txn] != 3) upRsp.push_back({responseDbid[txn], 0, 0});
          if (completed % 9 == 0 && nextSnp < 2100) {
            const unsigned snpTxn = nextSnp++;
            static constexpr unsigned snoopOps[] = {1, 3, 7, 9, 0xa, 0x11, 0x17};
            const unsigned snpOp = snoopOps[random() % 7];
            snoopAddress[snpTxn] = address[txn]; snp.push_back({snpTxn, address[txn], snpOp});
            if (snpOp == 0x11 || snpOp == 0x17) snoopAddress[snpTxn + 512] = address[txn];
          }
        }
      }
      if (dut.io_upSnp_valid && dut.io_upSnp_ready) {
        const unsigned op = dut.io_upSnp_bits_opcode;
        const uint64_t line = dut.io_upSnp_bits_addr >> 3;
        if (line == modifiedLine && clientDirty && op != 0) {
          const unsigned resp = (op < 2 ? 0U : 1U) | 4U;
          snoopData.push_back({dut.io_upSnp_bits_txnID, 0, 3, resp, 0, line});
          snoopData.push_back({dut.io_upSnp_bits_txnID, 1, 3, resp, 0, line});
          clientDirty = false;
        } else {
          upRsp.push_back({dut.io_upSnp_bits_txnID, 1, op < 2 ? 0U : 1U});
        }
      }
      if (dut.io_downTxDat_valid && dut.io_downTxDat_ready) {
        const unsigned txn = dut.io_downTxDat_bits_txnID, beat = dut.io_downTxDat_bits_dataID / 2;
        if (dut.io_downTxDat_bits_opcode == 2) {
          if (!writes.count(txn) || (writeMask[txn] & (1U << beat))) throw std::runtime_error("unknown/duplicate writeback");
          if (dut.io_downTxDat_bits_byteEnable) check(dut.io_downTxDat_bits_data, expectedData(writes[txn], beat), "writeback");
          writeMask[txn] |= 1U << beat;
          if (writeMask[txn] == 3) writes.erase(txn);
        } else {
          if (!snoopAddress.count(txn) || (snpMask[txn] & (1U << beat))) throw std::runtime_error("unknown/duplicate Snoop data");
          check(dut.io_downTxDat_bits_data, expectedData(snoopAddress[txn], beat), "Snoop data txn=" + std::to_string(txn) + " line=" + std::to_string(snoopAddress[txn]));
          if (dut.io_downTxDat_bits_opcode == 4) {
            if (dut.io_downTxDat_bits_tgtID != 12) throw std::runtime_error("misrouted DCT");
            ++dctBeats;
          } else if (dut.io_downTxDat_bits_tgtID != 8) throw std::runtime_error("misrouted Home Snoop data");
          snpMask[txn] |= 1U << beat;
          if (snpMask[txn] == 3) {
            if (dut.io_downTxDat_bits_opcode == 1) snoopAddress.erase(txn + 512);
            snoopAddress.erase(txn);
          }
        }
      }
      if (dut.io_downTxRsp_valid && dut.io_downTxRsp_ready) {
        const unsigned txn = dut.io_downTxRsp_bits_txnID;
        if (dut.io_downTxRsp_bits_opcode == 2) {
          if (!expectedCompAck.count(txn) || expectedCompAck[txn] != dut.io_downTxRsp_bits_tgtID)
            throw std::runtime_error("unknown or misrouted downstream CompAck");
          expectedCompAck.erase(txn);
        } else {
          if (!snoopAddress.count(txn)) throw std::runtime_error("unknown/duplicate Snoop response");
          if (dut.io_downTxRsp_bits_opcode == 1) snoopAddress.erase(txn + 512);
          snoopAddress.erase(txn);
        }
      }
      if (accepted == 131 && completed == 131 && wbfPhase == 3 && dat.empty() && rsp.empty() && upRsp.empty() &&
          snoopData.empty() && expectedCompAck.empty() && lateCompletion.empty() &&
          snp.empty() && writes.empty() && snoopAddress.empty() && !dut.io_downReq_valid && !dut.io_upSnp_valid)
        ++quiet; else quiet = 0;
      if (flushPhase && dut.io_upRsp_valid) throw std::runtime_error("internal Flush emitted an external completion");
      if (flushPhase == 0 && quiet == 40) { flushPhase = 1; flushCycles = 0; }
      else if (flushPhase == 1 && ++flushCycles == 100) { flushPhase = 2; flushCycles = 0; }
      else if (flushPhase == 2 && ++flushCycles == 5) { flushPhase = 3; flushCycles = 0; }
      else if (flushPhase == 3 && dut.io_flushDone) finished = true;
      tick();
    }
    std::printf("accepted=%u completed=%u downReads=%u writebacks=%u snoops=%u retries=%u dirtySnoopBeats=%u dctBeats=%u cycles=%u quiet=%u\n",
      accepted, completed, downReads, writebacks, snoops, retries, dirtySnoopBeats, dctBeats, cycle, quiet);
    if (!finished || snoops == 0 || retries == 0 || writebacks == 0 || dirtySnoopBeats != 2 || dctBeats == 0) return 1;
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "cycle=%u accepted=%u completed=%u: %s\n", cycle, accepted, completed, e.what());
    return 2;
  }
}
