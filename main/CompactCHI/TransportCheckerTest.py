"""Existing endpoint matrix plus bounded trace variants for routing invariants."""
import copy
import json
from pathlib import Path
import runpy
import sys

checker = runpy.run_path(str(Path(__file__).with_name("check-transport.py")))
root = Path(sys.argv[1])
results, accepted_variants, rejected = [], [], 0
sources = set()
paths = sorted(root.glob("*/canonical.jsonl"))
assert len(paths) == 30, "endpoint matrix case count changed"
for path in paths:
    rows = [json.loads(line) for line in path.read_text().splitlines()][1:]
    evidence = checker["check"](rows)
    rejected += checker["mutations"](rows)
    sources.update((r["direction"], r["opcode"]) for r in rows if r["handshake"])
    results.append(dict(case=path.parent.name, evidence=evidence))
    if evidence["transactions"] >= 2: assert evidence["peakLiveLines"] == 2
    if "fungible" in path.parent.name:
        assert evidence["retries"] == 2
        assert any(r["earlyCredit"] for r in evidence["retryEvidence"])
        assert evidence["stalls"].get("chi_rx_rsp", 0) > 0
    variants = []
    if evidence["retries"]:
        # TxnID does not identify the recipient of a fungible credit. Keep the
        # entire stalled offer consistent while varying this irrelevant field.
        changed = copy.deepcopy(rows)
        for r in changed:
            if r["direction"] == "chi_rx_rsp" and r["opcode"] == 7: r["txn_id"] = 4095
        checker["check"](changed)
        accepted_variants.append(dict(case=path.parent.name, variant="credit-txn-is-not-owner"))
        retry_request = next(r for r in rows if r["handshake"] and r["direction"] == "chi_tx_req" and not r["allow_retry"])
        for field, value in (("allow_retry", True), ("p_crd_type", (retry_request["p_crd_type"]+1)%16),
                             ("address", retry_request["address"]+64), ("mem_attr", retry_request["mem_attr"]^1)):
            changed = copy.deepcopy(rows)
            for r in changed:
                if r["direction"] == "chi_tx_req" and r["txn_id"] == retry_request["txn_id"] and not r["allow_retry"]:
                    r[field] = value
            variants.append(changed)
        for field in ("node", "p_crd_type"):
            changed = copy.deepcopy(rows)
            original = next(r for r in rows if r["handshake"] and r["direction"] == "chi_rx_rsp" and r["opcode"] == 7)
            for r in changed:
                if r["direction"] == "chi_rx_rsp" and r["opcode"] == 7 and r["node"] == original["node"] and r["p_crd_type"] == original["p_crd_type"]:
                    r[field] += 1
            variants.append(changed)
        changed = copy.deepcopy(rows)
        changed = [r for r in changed if not (r["direction"] == "chi_rx_rsp" and r["opcode"] == 3)]
        variants.append(changed)
    if any(r["direction"] == "chi_rx_rsp" and r["opcode"] == 11 for r in rows):
        # Existing RTL routes separated completion from RSP even when its DBID
        # differs from DAT and its TraceTag disagrees with all DAT beats.
        changed = copy.deepcopy(rows)
        for r in changed:
            if r["direction"] == "chi_rx_rsp" and r["opcode"] == 11: r["dbid"] += 1000
            elif r["direction"] == "chi_tx_rsp" and r["opcode"] == 2: r["txn_id"] += 1000
            elif r["direction"] == "chi_rx_dat": r["trace_tag"] = not r["trace_tag"]
        checker["check"](changed)
        accepted_variants.append(dict(case=path.parent.name, variant="separated-rsp-owns-dbid-and-tag"))
        changed = [copy.deepcopy(r) for r in rows if not (r["direction"] == "chi_rx_rsp" and r["opcode"] == 11)]
        variants.append(changed)
    elif (path.parent.name.startswith("read-0") and path.parent.name.endswith("tag-0")) or path.parent.name == "fungible-outliving-credit":
        changed = copy.deepcopy(rows)
        dbid = next(r["dbid"] for r in rows if r["direction"] == "chi_rx_dat" and r["txn_id"] == 70)
        for r in changed:
            if r["direction"] == "chi_rx_dat" and r["txn_id"] == 70 and r["data_id"] == 0: r["trace_tag"] = True
            elif r["direction"] == "chi_tx_rsp" and r["txn_id"] == dbid: r["trace_tag"] = True
        checker["check"](changed)
        accepted_variants.append(dict(case=path.parent.name, variant="combined-accumulates-both-dat-tags"))
    if any(r["direction"] == "chi_tx_rsp" for r in rows):
        changed = copy.deepcopy(rows)
        ack = next(r for r in changed if r["handshake"] and r["direction"] == "chi_tx_rsp")
        request = next(r for r in rows if r["direction"] == "chi_tx_req")
        ack["cycle"] = request["cycle"]
        changed.sort(key=lambda r:r["cycle"]); variants.append(changed)
    if path.parent.name == "reused-acquire-victim-txn":
        changed = copy.deepcopy(rows)
        acks = [r for r in changed if r["direction"] == "chi_tx_rsp"]
        acks[0]["txn_id"], acks[1]["txn_id"] = acks[1]["txn_id"], acks[0]["txn_id"]
        variants.append(changed)
    for direction, opcode in (("chi_rx_dat",4),("chi_rx_dat",11),("cchi_rx_dat",2)):
        accepted = [r for r in rows if r["handshake"] and r["direction"] == direction and r["opcode"] == opcode]
        if accepted:
            victim = accepted[-1]
            changed = [copy.deepcopy(r) for r in rows if r is not victim]
            variants.append(changed)
    for changed in variants:
        try: checker["check"](changed)
        except (AssertionError, KeyError, IndexError, TypeError, ValueError): rejected += 1
        else: raise AssertionError(path.parent.name + " accepted a bad retry/completion order")

# Cross-line DAT interleave is a trace-audit fixture, separate from the Home's
# FIFO data offers. Both lines already have independent accepted request intent.
base = [json.loads(line) for line in (root/"read-0-retry-0-tag-0/canonical.jsonl").read_text().splitlines()][1:]
compact = [copy.deepcopy(r) for r in base if r["handshake"]]
requests = [r for r in compact if r["direction"] == "chi_tx_req"]
beats = {txn: [r for r in compact if r["direction"] == "chi_rx_dat" and r["txn_id"] == txn] for txn in (70,71)}
acks = [r for r in compact if r["direction"] == "chi_tx_rsp"]
interleaved = requests + [beats[70][0],beats[71][0],beats[70][1],beats[71][1]] + acks
for cycle,r in enumerate(interleaved,1): r["cycle"]=cycle; r["sim_time"]=2*cycle
checker["check"](interleaved)
accepted_variants.append(dict(case="two-lines",variant="alternating-dat-beats"))

def sample(cycle, direction, node, target, txn, opcode, **fields):
    row = dict(cycle=cycle, sim_time=2*cycle, direction=direction, node=node, target=target,
               txn_id=txn, opcode=opcode, valid=True, ready=True, handshake=True, trace_tag=False,
               address=None, dbid=None, data_id=None, data=None, resp=0, resp_err=0,
               allow_retry=None, exp_comp_ack=None, p_crd_type=0, size=None, mem_attr=None,
               order=None, ns=None, likely_shared=None, home_node=None, byte_enable=None)
    row.update(fields)
    return row

# Split MMIO response orders already supported by MMIOBridge, without adding
# them to EbHome's production stimulus. DBID and Comp remain separate obligations.
for order in ("comp-before-dbid", "data-before-comp", "comp-with-final-data"):
    req = sample(1,"chi_tx_req",1,8,2048,28,address=0x40008,allow_retry=False,
                 exp_comp_ack=False,size=3,mem_attr=0,order=0,ns=False,likely_shared=False)
    dbid = sample(3 if order == "comp-before-dbid" else 2,"chi_rx_rsp",8,1,2048,6,dbid=303,trace_tag=True)
    dat = sample(4 if order == "comp-before-dbid" else 3,"chi_tx_dat",1,8,303,3,
                 data_id=0,data="a5"*32,byte_enable=0xff00,trace_tag=True)
    comp = sample(2 if order == "comp-before-dbid" else 4 if order == "data-before-comp" else 3,
                  "chi_rx_rsp",14,1,2048,4,dbid=999)
    rows = sorted([req,dbid,comp,dat],key=lambda r:r["cycle"])
    checker["check"](rows)
    accepted_variants.append(dict(case="mmio-split-write",variant=order))
    bad = copy.deepcopy(rows)
    next(r for r in bad if r["direction"] == "chi_tx_dat")["txn_id"] = 999
    try: checker["check"](bad)
    except AssertionError: rejected += 1
    else: raise AssertionError("MMIO DAT used Comp DBID instead of DBIDResp")
req = sample(1,"chi_tx_req",1,8,2048,4,address=0x40020,allow_retry=False,
             exp_comp_ack=False,size=3,mem_attr=0,order=0,ns=False,likely_shared=False)
dat = sample(2,"chi_rx_dat",8,1,2048,11,dbid=101,home_node=8,data_id=2,data="00"*32,trace_tag=True)
rsp = sample(3,"chi_rx_rsp",14,1,2048,11,dbid=202)
checker["check"]([req,dat,rsp])
accepted_variants.append(dict(case="mmio-split-read",variant="upper-beat-before-rsp"))
try: checker["check"]([req,sample(2,"chi_rx_rsp",8,1,2048,3,p_crd_type=3),dat,rsp])
except AssertionError: rejected += 1
else: raise AssertionError("MMIO Retry was accepted despite the no-retry contract")

state = dict(state="passed", endpointCases=len(results), traceVariants=len(accepted_variants),
             mutationRejections=rejected, results=results, acceptedVariants=accepted_variants,
             observedSources=[dict(direction=d,opcode=o) for d,o in sorted(sources)],
             scope="existing Home/Requester accepted matrix and explicit host trace variants; no DUT/Directory/complete-topology equivalence")
(root/"transport-host.json").write_text(json.dumps(state,indent=2)+"\n")
print("Transport checker:",len(results),"endpoint cases,",len(accepted_variants),"trace variants and",rejected,"mutations passed")
