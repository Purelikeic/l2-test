"""Independent configured-node and action/transport checks on observed port events."""
import configparser
import json
from pathlib import Path


def topology(root):
    topo = json.loads((root / "topology.json").read_text())
    if (root / "cchitest.ini").exists():
        ini = configparser.ConfigParser()
        ini.read(root / "cchitest.ini")
        section = ini["tltest.config"] if ini.has_section("tltest.config") else {}
        counts = [int(section.get(k, default), 0) for k, default in (("core", "1"), ("core.tl_c", "1"), ("core.tl_ul", "0"))]
        assert counts == [topo["coreCount"], topo["tlcPerCore"], topo["tlulPerCore"]]
    expected = []
    for core in range(topo["coreCount"]):
        for kind, count in ((0, topo["tlcPerCore"]), (1, topo["tlulPerCore"])):
            for slot in range(count):
                expected.append((len(expected), core, slot, kind))
    for core in range(topo["coreCount"]):
        expected.append((len(expected), core, 0, 2))
    assert [(n["id"], n["core"], n["slot"], n["kind"]) for n in topo["nodes"]] == expected
    return topo


def check(root, events, seed, action_mode=False):
    topo = topology(root)
    nodes = {n["id"]: n["kind"] for n in topo["nodes"]}
    actions = [json.loads(line) for line in (root / "actions.jsonl").read_text().splitlines()]
    expected_requests = []
    for a in actions:
        assert a["node"] in nodes and a["kind"] < 12
        role = nodes[a["node"]]
        assert (role == 0 and a["kind"] in (*range(9), 11)) or (role == 1 and a["kind"] == 8) or (role == 2 and a["kind"] in (9, 10))
        if a["kind"] == 11:
            continue
        direction = "cchi_rx_evt" if a["kind"] in (3, 4) else "cchi_rx_req"
        code = {0: 4, 1: 16, 2: 18, 3: 0, 4: 1, 5: 12, 6: 13, 7: 14, 8: 3, 9: 2, 10: 9 if a["size"] == 6 else 8}[a["kind"]]
        expected_requests.append((direction, a["node"], a["txn"], code, a["address"], a["trace_tag"]))
    fired = [r for r in events if r["handshake"]]
    observed = [(r["direction"], r["node"], r["txn_id"], r["opcode"], r["address"], r["trace_tag"]) for r in fired
                if r["direction"] in ("cchi_rx_req", "cchi_rx_evt")]
    for direction in ("cchi_rx_req", "cchi_rx_evt"):
        assert [r for r in observed if r[0] == direction] == [r for r in expected_requests if r[0] == direction]
    assert all(nodes[r["target"]] == 0 for r in fired if r["direction"] == "cchi_tx_snp")
    if not action_mode:
        return {"configuredNodes": len(nodes), "acceptedActions": len(actions)}
    # All sixteen actions are one bounded, independently specified API program.
    assert [a["kind"] for a in actions] == [0, 1, 11, 4, 2, 5, 6, 0, 3, 8, 1, 11, 7, 8, 10, 9]
    by_txn = {a["txn"]: a for a in actions}
    original = bytes(((topo["memoryStart"] >> 6) ^ (i * 17) ^ seed) & 255 for i in range(64))
    overwrite = bytes.fromhex(by_txn[24]["data"])
    expected_reads = {20: original, 21: original, 27: overwrite, 29: overwrite, 30: overwrite, 33: overwrite}
    for txn, data in expected_reads.items():
        rows = [r for r in fired if r["direction"] == "cchi_tx_dat" and r["txn_id"] == txn]
        assert len(rows) == 2 and {r["data_id"] for r in rows} == {0, 1}
        for row in rows:
            assert row["target"] == by_txn[txn]["node"] and row["trace_tag"] == by_txn[txn]["trace_tag"] and row["resp_err"] == 0
            beat = row["data_id"]
            assert bytes.fromhex(row["data"]) == data[beat * 32:(beat + 1) * 32]
    for txn, opcode in ((24, 1), (25, 4), (26, 4), (28, 1), (32, 4), (34, 1)):
        rows = [r for r in fired if r["direction"] == "cchi_tx_rsp" and r["txn_id"] == txn and r["opcode"] == opcode]
        assert len(rows) == 1 and rows[0]["target"] == by_txn[txn]["node"]
        assert rows[0]["trace_tag"] == by_txn[txn]["trace_tag"] and rows[0]["resp_err"] == 0
    for txn, opcode in ((23, 3), (34, 2)):
        response = [r for r in fired if r["direction"] == "cchi_tx_rsp" and r["txn_id"] == txn and r["opcode"] == opcode]
        assert len(response) == 1
        response = response[0]
        data = [r for r in fired if r["direction"] == "cchi_rx_dat" and r["node"] == by_txn[txn]["node"] and r["txn_id"] == response["dbid"] and r["opcode"] == (2 if txn == 23 else 0)]
        assert len(data) == (2 if txn == 23 else 1)
        expected = bytes.fromhex(by_txn[22 if txn == 23 else 34]["data"])
        for row in data:
            assert row["cycle"] > response["cycle"] and row["trace_tag"] == response["trace_tag"]
            assert row["byte_enable"] == (0xffffffff if txn == 23 else 0xff)
            beat = row["data_id"]
            assert bytes.fromhex(row["data"])[:32 if txn == 23 else 8] == expected[beat * 32:beat * 32 + (32 if txn == 23 else 8)]
    device = [r for r in fired if r["direction"] == "cchi_tx_dat" and r["txn_id"] == 35]
    assert len(device) == 1 and device[0]["target"] == by_txn[35]["node"] and device[0]["data_id"] == 0
    assert bytes.fromhex(device[0]["data"])[:8] == bytes.fromhex(by_txn[34]["data"])[:8]
    # Flush must recall the SC client retained by the preceding CleanShared.
    assert any(r["direction"] == "cchi_tx_snp" and r["address"] == by_txn[26]["address"] and r["opcode"] == 1 for r in fired)
    assert any(r["direction"] == "chi_tx_req" and r["opcode"] == 12 and r["address"] == by_txn[24]["address"] for r in fired)
    assert any(r["direction"] == "chi_tx_req" and r["opcode"] == 23 for r in fired)
    return {"configuredNodes": len(nodes), "acceptedActions": len(actions), "cacheReads": len(expected_reads),
            "permissionOverwrite": True, "writebackCombined": True, "cmoKinds": 3, "mmioReadback": True}
