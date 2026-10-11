#!/usr/bin/env python3
"""Check the initial DUT read profile and align every observation to real VCD ports."""
import json
import runpy
from pathlib import Path
import sys

root = Path(sys.argv[1])
randomized = sys.argv[2:] == ["random-read"]
lower_profile = sys.argv[2:] == ["lower-snoop"]
victim_profile = sys.argv[2:] == ["victim"]
profile = sys.argv[2] if len(sys.argv) > 2 else "deterministic-read"
if profile == "configured" and (root / "scenario.json").exists():
    profile = {0: "configured", 1: "deterministic-read", 2: "random-read", 3: "lower-snoop", 4: "victim", 5: "node-actions", 6: "deterministic-read"}[json.loads((root / "scenario.json").read_text())]
randomized, lower_profile, victim_profile = profile == "random-read", profile == "lower-snoop", profile == "victim"
scopes = runpy.run_path(str(Path(__file__).with_name("profile_scopes.py")))["SCOPES"]
rows = [json.loads(line) for line in (root / "canonical.jsonl").read_text().splitlines()]
seed = rows[0]["seed"]
events = rows[1:]
assert all(r["sim_time"] == 2 * r["cycle"] for r in events)
if profile != "configured":
    assert {r["trace_tag"] for r in events if r["direction"] == "cchi_rx_req"} == {False, True}
assert [r["cycle"] for r in events] == sorted(r["cycle"] for r in events)
node_check = runpy.run_path(str(Path(__file__).with_name("check-node-events.py")))
topo = node_check["topology"](root)
base = topo["memoryStart"]
coherent = [n["id"] for n in topo["nodes"] if n["kind"] == 0 and n["mode"] != 0]
transient = [n["id"] for n in topo["nodes"] if n["kind"] == 1 and n["mode"] != 0]
node_evidence = node_check["check"](root, events, seed, action_mode=profile == "node-actions")
if profile in ("node-actions", "configured"):
    expected_requests = sum(r["direction"] == "cchi_rx_req" and r["handshake"] for r in events)
    latency = {}
    victim_evidence = None
else:
    requests = {}
    held = {}
    for row in events:
        direction = row["direction"]
        fields = {key: value for key, value in row.items()
                  if key not in ("cycle", "sim_time", "ready", "handshake")}
        if direction in held:
            previousCycle, previousFields = held[direction]
            assert row["cycle"] == previousCycle + 1 and fields == previousFields, (
                direction, row["cycle"], "offered valid/flit changed under stall")
        if row["valid"] and not row["ready"]: held[direction] = row["cycle"], fields
        else: held.pop(direction, None)
    assert not held, "terminal stalled flit was not consumed"
    beats = {}
    acks = {}
    latency = {}
    for row in events:
        if not row["handshake"]:
            continue
        direction = row["direction"]
        if direction == "cchi_rx_req":
            key = row["node"], row["txn_id"]
            assert key not in requests
            requests[key] = row
        elif direction == "cchi_tx_dat":
            key = row["target"], row["txn_id"]
            req = requests[key]
            beat = row["data_id"]
            assert beat in (0, 1) and beat not in beats.setdefault(key, set())
            beats[key].add(beat)
            expected = bytearray(((req["address"] >> 6) ^ (i * 17) ^ seed) & 255 for i in range(64))
            if victim_profile and req["opcode"] == 3 and req["address"] == base:
                expected[0] ^= 0xa5
                expected[63] ^= 0x5a
            assert bytes.fromhex(row["data"]) == expected[beat * 32:(beat + 1) * 32]
            assert row["trace_tag"] == req["trace_tag"] and row["resp_err"] == 0
            if req["opcode"] == 3:
                assert row["resp"] in (0, 2)
            elif req["opcode"] == 4:
                assert row["resp"] in (1, 2, 6)
                acks[row["target"], row["dbid"]] = key
            elif req["opcode"] == 16:
                assert row["resp"] in (2, 6)
                acks[row["target"], row["dbid"]] = key
            if beats[key] == {0, 1}:
                latency[str(key)] = row["cycle"] - req["cycle"]
        elif direction == "cchi_rx_rsp" and row["opcode"] == 0:
            key = acks.pop((row["node"], row["txn_id"]))
            assert beats[key] == {0, 1}
    expected_requests = 10 if victim_profile else 96 if randomized else 5 if lower_profile else 4
    assert len(requests) == expected_requests and len(beats) == expected_requests and not acks
    if randomized:
        assert {key[0] for key in requests} == set(coherent[:2] + transient[:2])
        for (node, txn), request in requests.items():
            if node in transient: assert request["opcode"] == 3
            else: assert request["opcode"] == 16 and request["address"] == base + coherent.index(node) * 64
    assert all(b == {0, 1} for b in beats.values())
    # Assert that intended separated completion and retry stimulus actually fired.
    if not victim_profile:
        assert any(r["direction"] == "chi_rx_rsp" and r["opcode"] == 11 and r["handshake"] for r in events)
        assert any(r["direction"] == "chi_rx_rsp" and r["opcode"] == 3 and r["handshake"] for r in events)
        assert any(r["direction"] == "chi_rx_rsp" and r["opcode"] == 7 and r["handshake"] for r in events)
    victim_evidence = None
    if victim_profile:
        victim_evidence = runpy.run_path(str(Path(__file__).with_name("check-victim-events.py")))["check"](events, seed, topo)

    if lower_profile:
        lower_requests = [r for r in events if r["direction"] == "chi_rx_snp" and r["handshake"]]
        assert [(r["txn_id"], r["opcode"]) for r in lower_requests] == [(2048, 1), (2049, 23)]
        dirty = bytearray((((base + 64) >> 6) ^ (i * 17) ^ seed) & 255 for i in range(64))
        dirty[0] ^= 0xa5
        home_data = [r for r in events if r["direction"] == "chi_tx_dat" and r["target"] == 8 and r["handshake"]]
        dct_data = [r for r in events if r["direction"] == "chi_tx_dat" and r["target"] == 12 and r["handshake"]]
        assert len(home_data) == len(dct_data) == 2
        for records, txn, opcode, resp in ((home_data, 2048, 1, 5), (dct_data, 2500, 4, 2)):
            assert {r["data_id"] for r in records} == {0, 2}
            for row in records:
                assert row["txn_id"] == txn and row["opcode"] == opcode and row["resp"] == resp
                beat = row["data_id"] // 2
                assert bytes.fromhex(row["data"]) == dirty[beat * 32:(beat + 1) * 32]
        assert all(r["dbid"] == 2049 and r["home_node"] == 8 for r in dct_data)
        assert all(r["trace_tag"] is True for r in home_data + dct_data + lower_requests)
        forwarded = [r for r in events if r["direction"] == "chi_tx_rsp" and r["txn_id"] == 2049 and r["handshake"]]
        assert len(forwarded) == 1 and forwarded[0]["opcode"] == 9 and forwarded[0]["resp"] == 0 and forwarded[0]["forward_state"] == 2

if lower_profile:
    forward_rows = [json.loads(line) for line in (root / "forward.jsonl").read_text().splitlines()]
    dct_rows = [r for r in events if r["handshake"] and r["direction"] == "chi_tx_dat" and r["target"] == 12 and r["opcode"] == 4]
    assert len(forward_rows) == 3 and len(dct_rows) == 2
    for f, d in zip(forward_rows[:2], sorted(dct_rows, key=lambda r: r["cycle"])):
        assert f["kind"] == "data" and f["cycle"] == d["cycle"] and f["txn"] == d["txn_id"]
        assert f["dbid"] == d["dbid"] and f["source"] == d["node"] and f["target"] == d["target"]
        assert f["beat"] == d["data_id"] // 2 and f["trace_tag"] == d["trace_tag"]
    ack = forward_rows[2]
    assert ack["kind"] == "ack" and ack["cycle"] == max(r["cycle"] for r in dct_rows) + 1
    assert ack["txn"] == dct_rows[0]["dbid"] and ack["dbid"] == dct_rows[0]["dbid"]
    assert ack["source"] == 12 and ack["target"] == 8 and ack["trace_tag"] == dct_rows[0]["trace_tag"]

forward_tools = runpy.run_path(str(Path(__file__).with_name("check-forward.py")))
forward_evidence = forward_tools["check"](root, events, profile)
forward_evidence["mutationRejections"] = forward_tools["mutations"](root, events, profile)
architectural_tools = runpy.run_path(str(Path(__file__).with_name("check-architectural-state.py")))
architectural_evidence = architectural_tools["check"](root, events, seed, topo)
architectural_evidence["mutationRejections"] = architectural_tools["mutations"](root, events, seed, topo)
resource_tools = runpy.run_path(str(Path(__file__).with_name("check-resources.py")))
resource_rows = [json.loads(line) for line in (root / "resources.jsonl").read_text().splitlines()]
assert json.loads((root / "architectural-state.json").read_text())["cycle"] == resource_rows[-1]["cycle"], "architectural observation is not at the terminal resource sample"
resource_evidence = resource_tools["check"](resource_rows)
resource_mutations = resource_tools["mutations"](resource_rows)
resource_by_time = {row["sim_time"]: row for row in resource_rows}
assert len(resource_by_time) == len(resource_rows)
resource_fields = resource_tools["FIELDS"]
directory_tools = runpy.run_path(str(Path(__file__).with_name("check-directory.py")))
directory_rows = [json.loads(line) for line in (root / "directory.jsonl").read_text().splitlines()]
data_rows = [json.loads(line) for line in (root / "datastorage.jsonl").read_text().splitlines()]
assert directory_rows[0] == data_rows[0] and len(directory_rows) == len(data_rows)
expected_clients = {(e["node"], e["address"]): e["clientState"] for e in architectural_evidence["requesterFootprint"]}
roles = {n["id"]: n["kind"] for n in topo["nodes"]}
directory_evidence = directory_tools["check"](directory_rows, resource_rows, expected_clients, roles, data_rows[-1]["cycle"])
directory_evidence["mutationRejections"] = directory_tools["mutations"](directory_rows, resource_rows, expected_clients, roles)
data_tools = runpy.run_path(str(Path(__file__).with_name("check-datastorage.py")))
data_evidence = data_tools["check"](directory_rows, data_rows, resource_rows, architectural_evidence["l2DataVersions"], architectural_evidence["l2UndefinedStorage"], architectural_evidence["l2Transitions"])
data_evidence["mutationRejections"] = data_tools["mutations"](directory_rows, data_rows, resource_rows, architectural_evidence["l2DataVersions"], architectural_evidence["l2UndefinedStorage"], architectural_evidence["l2Transitions"])
assert not any(r["cycle"] >= directory_rows[1]["cycle"] for r in events), "Directory/DataStorage scan overlaps live channel traffic"
directory_by_time = {row["sim_time"]: row for row in directory_rows[1:]}
data_by_time = {row["sim_time"]: row for row in data_rows[1:]}
directory_by_coordinate = {(row["set"], row["way"]): row for row in directory_rows[1:]}
directory_geometry = directory_rows[0]
ports = {
    "cchi_rx_req": "req", "cchi_rx_evt": "evt", "cchi_rx_rsp": "upRspIn", "cchi_rx_dat": "upDatIn",
    "cchi_tx_snp": "upSnp", "cchi_tx_rsp": "upRsp", "cchi_tx_dat": "upDat",
    "chi_tx_req": "downReq", "chi_rx_rsp": "downRsp", "chi_rx_dat": "downDat",
    "chi_rx_snp": "downSnp", "chi_tx_rsp": "downTxRsp", "chi_tx_dat": "downTxDat",
}
by_time = {}
for row in events:
    by_time.setdefault(row["sim_time"], []).append(row)
symbols = {}
values = {}
time = None
checked = 0
resource_checked = 0
directory_checked = 0
data_checked = 0

def check_time():
    global checked, resource_checked, directory_checked, data_checked
    if time in directory_by_time:
        row = directory_by_time.pop(time)
        expected = {"io_directorySet": row["set"], "io_directoryWay": row["way"]}
        for key, signal in (("sets", "directorySets"), ("ways", "directoryWays"), ("slices", "directorySlices"),
                            ("lineBytes", "directoryLineBytes"), ("addrBits", "directoryAddrBits")):
            expected["io_" + signal] = directory_geometry[key]
        for slice_id, entry in enumerate(row["entries"]):
            for key in ("tag", "state", "client", "clientState", "clientID"):
                expected[f"io_directoryEntries_{slice_id}_{key}"] = int(entry[key])
        for signal, value in expected.items():
            assert signal in symbols and values.get(symbols[signal]) == value, (time, signal, "Directory log differs from actual storage VCD")
        assert values[symbols["clock"]] == 0
        directory_checked += 1
    if time in data_by_time:
        row = data_by_time.pop(time)
        for key in ("set", "way"):
            signal = "io_directory" + key.capitalize()
            assert values.get(symbols[signal]) == row[key], (time, signal, "DataStorage selector differs from VCD")
        stable_meta = directory_by_coordinate[row["set"], row["way"]]
        for slice_id, entry in enumerate(stable_meta["entries"]):
            for key in ("tag", "state", "client", "clientState", "clientID"):
                signal = f"io_directoryEntries_{slice_id}_{key}"
                assert values.get(symbols[signal]) == int(entry[key]), (time, signal, "Directory changed between terminal sweeps")
        for slice_id, line in enumerate(row["lines"]):
            signal = f"io_dataStorageLines_{slice_id}"
            assert signal in symbols and values.get(symbols[signal]) == int.from_bytes(bytes.fromhex(line["data"]), "little"), (time, signal, "DataStorage log differs from actual storage VCD")
            error = f"io_dataStorageErrors_{slice_id}"
            assert error in symbols and values.get(symbols[error]) == int(line["error"]), (time, error, "DataStorage error differs from VCD")
        assert values[symbols["clock"]] == 0
        data_checked += 1
    if time in resource_by_time:
        row = resource_by_time.pop(time)
        for field in (*resource_fields, "ready"):
            signal = "io_resources_" + field
            assert signal in symbols, signal
            assert values.get(symbols[signal]) == int(row[field]), (time, signal, "resource log differs from DUT VCD")
        assert values[symbols["clock"]] == 0
        resource_checked += 1
    if time not in by_time:
        return
    for row in by_time.pop(time):
        prefix = "io_" + ports[row["direction"]]
        expected = {prefix + "_valid": int(row["valid"]), prefix + "_ready": int(row["ready"]),
                    prefix + "_bits_txnID": row["txn_id"], prefix + "_bits_srcID": row["node"],
                    prefix + "_bits_opcode": row["opcode"]}
        for field, suffix in (("dbid", "dbID"), ("data_id", "dataID"), ("resp", "resp"),
                              ("resp_err", "respErr"), ("data_source", "dataSource"),
                              ("forward_state", "fwdState"), ("home_node", "homeNID"),
                              ("forward_node", "fwdNID"), ("forward_txn", "fwdTxnID"), ("byte_enable", "byteEnable")):
            if row[field] is not None:
                expected[prefix + "_bits_" + suffix] = row[field]
        if prefix + "_bits_tgtID" in symbols:
            expected[prefix + "_bits_tgtID"] = row["target"]
        if prefix + "_bits_traceTag" in symbols:
            expected[prefix + "_bits_traceTag"] = int(row["trace_tag"])
        if row["address"] is not None:
            address = row["address"] >> 3 if row["direction"].endswith("snp") else row["address"]
            expected[prefix + "_bits_addr"] = address
        if row["data"] is not None:
            expected[prefix + "_bits_data"] = int.from_bytes(bytes.fromhex(row["data"]), "little")
        for signal, value in expected.items():
            assert signal in symbols, signal
            assert values.get(symbols[signal]) == value, (time, signal, values.get(symbols[signal]), value)
        assert values[symbols["clock"]] == 0, (time, "observation not sampled before active edge")
        checked += 1

with (root / "waveform.vcd").open() as waveform:
    scope = []
    header = True
    ids = set()
    for line in waveform:
        fields = line.strip().split()
        if header:
            if fields and fields[0] == "$scope": scope.append(fields[2])
            elif fields and fields[0] == "$upscope": scope.pop()
            elif fields and fields[0] == "$var" and scope == ["TOP"]:
                symbols[fields[4]] = fields[3]
            elif fields and fields[0] == "$enddefinitions":
                ids = set(symbols.values())
                header = False
            continue
        if not fields: continue
        value = fields[0]
        if value.startswith("#"):
            check_time()
            time = int(value[1:])
        elif value[0] in "01xzXZ":
            symbol = value[1:]
            if symbol in ids: values[symbol] = int(value[0]) if value[0] in "01" else None
        elif value[0] == "b" and len(fields) == 2 and fields[1] in ids:
            values[fields[1]] = int(value[1:], 2) if not any(c in value.lower() for c in "xz") else None
    check_time()
assert not by_time and checked == len(events)
assert not resource_by_time and resource_checked == len(resource_rows)
assert not directory_by_time and directory_checked == len(directory_rows) - 1
assert not data_by_time and data_checked == len(data_rows) - 1
directory_evidence["waveformAlignedSlots"] = directory_checked * directory_geometry["slices"]
data_evidence["waveformAlignedSlots"] = data_checked * directory_geometry["slices"]
resource_evidence["waveformAlignedSamples"] = resource_checked
resource_evidence["mutationRejections"] = resource_mutations
evidence = {"profile": profile, "seed": seed,
            "requestCount": expected_requests,
            "waveformAlignedEvents": checked, "responseLatencyCycles": latency,
            "scope": scopes[profile], "nodeEvidence": node_evidence, "resourceEvidence": resource_evidence,
            "architecturalEvidence": architectural_evidence, "directoryEvidence": directory_evidence,
            "dataStorageEvidence": data_evidence, "forwardEvidence": forward_evidence}
if victim_evidence is not None:
    evidence["victimEvidence"] = victim_evidence
if latency:
    samples = sorted(latency.values())
    evidence["latencySummaryCycles"] = {"min": samples[0], "max": samples[-1],
        "p50": samples[(len(samples) - 1) // 2], "p95": samples[(len(samples) - 1) * 95 // 100]}
(root / "trace-evidence.json").write_text(json.dumps(evidence, indent=2))
print(f"seed={seed}: independent {expected_requests}-REQ data/obligations and {checked} exact VCD samples passed; latency={evidence.get('latencySummaryCycles')}")
