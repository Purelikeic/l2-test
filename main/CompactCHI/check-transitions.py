"""Audit generated transition identities/order and bounded terminal state.

Input versions come from the independent architectural replay. No Directory tag,
way, resource count or DUT-returned byte creates an expected version here.
"""
import copy
from collections import Counter, defaultdict

INSTALL = {"refill", "snoop-data", "upstream-writeback-install"}
DISCARD = {"victim-writeback-discard", "victim-evict-discard", "cmo-evict-discard",
           "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard"}
CMO = {"cmo-cleanshared-preserve": 5, "cmo-cleaninvalid-discard": 6, "cmo-makeinvalid-discard": 7}
ACTION_OP = {0: 4, 1: 16, 2: 18, 3: 0, 4: 1, 5: 12, 6: 13, 7: 14, 8: 3}


def check(events, actions, architectural, directory, records=None):
    records = architectural["l2TransitionEvents"] if records is None else records
    projection, current, last = defaultdict(list), {}, {}
    indices = []
    for t in records:
        address, kind = t["address"], t["kind"]
        assert address % 64 == 0
        projection[address].append(kind)
        if kind == "initial":
            assert address not in current and t["eventIndex"] is None and t["originIndex"] is None
            assert t["cycle"] is None and t["actionIndex"] is None and t["data"] is None and t["payload"] is None
            current[address] = None
            continue
        assert address in current
        index, origin_index = t["eventIndex"], t["originIndex"]
        assert 0 <= origin_index <= index < len(events)
        r, origin = events[index], events[origin_index]
        assert r["handshake"] and origin["handshake"] and r["cycle"] == t["cycle"]
        assert origin["cycle"] <= r["cycle"] and origin["address"] == address
        indices.append(index)
        action = None
        if t["actionIndex"] is not None:
            action = actions[t["actionIndex"]]
            request_index = t["actionRequestIndex"]
            assert 0 <= request_index <= origin_index
            request = events[request_index]
            assert request["handshake"] and request["target"] == 1 and request["cycle"] > action["cycle"]
            assert request["direction"] == ("cchi_rx_evt" if action["kind"] in (3, 4) else "cchi_rx_req")
            assert (request["node"], request["txn_id"], request["address"], request["opcode"], request["trace_tag"]) == (
                action["node"], action["txn"], address, ACTION_OP[action["kind"]], action["trace_tag"])
        else:
            assert t["actionRequestIndex"] is None
        if kind in CMO or kind in ("evict-client-release", "makeunique-no-data"):
            wanted = CMO.get(kind, 3 if kind == "evict-client-release" else 2)
            assert action is not None and action["kind"] == wanted and origin_index == t["actionRequestIndex"]
            if kind == "makeunique-no-data":
                assert r["direction"] == "cchi_rx_rsp" and r["opcode"] == 0 and index > origin_index
                assert r["node"] == origin["node"] and r["target"] == origin["target"]
                grants = [e for e in events[origin_index+1:index] if e["handshake"] and
                          e["direction"] == "cchi_tx_rsp" and e["node"] == origin["target"] and
                          e["target"] == origin["node"] and e["txn_id"] == origin["txn_id"]]
                assert len(grants) == 1 and grants[0]["opcode"] == 1 and grants[0]["resp"] == 2
                assert grants[0]["dbid"] == r["txn_id"] and r["trace_tag"] == grants[0]["trace_tag"] == origin["trace_tag"]
                home_requests = [e for e in events[origin_index+1:index] if e["handshake"] and
                                 e["direction"] == "chi_tx_req" and e["opcode"] == 12 and e["address"] == address]
                if home_requests:
                    home_request = home_requests[-1]
                    home_done = [e for e in events[origin_index+1:index] if e["handshake"] and
                                 e["direction"] == "chi_rx_rsp" and e["opcode"] == 4 and
                                 (e["node"], e["target"], e["txn_id"]) ==
                                 (home_request["target"], home_request["node"], home_request["txn_id"])]
                    assert len(home_done) == 1 and home_request["cycle"] < home_done[0]["cycle"] < grants[0]["cycle"]
            else:
                assert r["direction"] == "cchi_tx_rsp" and r["node"] == origin["target"]
                assert r["target"] == origin["node"] and r["txn_id"] == origin["txn_id"]
                assert r["opcode"] == (4 if kind in CMO else 1) and r["resp_err"] == 0
                assert r["trace_tag"] == origin["trace_tag"] and index > origin_index
        elif kind in ("snoop-data", "snoop-data-transient", "snoop-no-data"):
            assert origin["direction"] == "cchi_tx_snp" and origin["opcode"] in (0, 1, 2, 3)
            assert r["node"] == origin["target"] and r["target"] == origin["node"] and r["txn_id"] == origin["txn_id"]
            assert r["trace_tag"] == origin["trace_tag"] and r["resp_err"] == 0 and index > origin_index
            if kind == "snoop-no-data":
                assert r["direction"] == "cchi_rx_rsp" and r["opcode"] == 1
            else:
                assert bool(r["resp"] & 3) == (kind == "snoop-data")
                assert r["direction"] == "cchi_rx_dat" and r["opcode"] == 3
        elif kind == "upstream-writeback-install":
            assert action is not None and action["kind"] == 4 and origin["direction"] == "cchi_rx_evt"
            assert r["direction"] == "cchi_rx_dat" and r["opcode"] == 2 and r["node"] == origin["node"]
            assert r["target"] == origin["target"] and r["resp"] == 6 and r["trace_tag"] == origin["trace_tag"]
            dbids = [e for e in events[origin_index+1:index] if e["handshake"] and
                     e["direction"] == "cchi_tx_rsp" and e["node"] == origin["target"] and
                     e["target"] == origin["node"] and e["txn_id"] == origin["txn_id"] and e["opcode"] in (2, 3)]
            assert len(dbids) == 1 and r["txn_id"] == dbids[0]["dbid"]
        elif kind == "refill":
            assert origin["direction"] == "chi_tx_req" and origin["opcode"] in (7, 38)
            assert r["direction"] == "chi_rx_dat" and r["opcode"] in (4, 11)
            assert r["target"] == origin["node"] and r["txn_id"] == origin["txn_id"] and r["resp_err"] == 0
        elif kind in ("victim-evict-discard", "cmo-evict-discard"):
            assert origin["direction"] == "chi_tx_req" and origin["opcode"] in (13, 66)
            assert r["direction"] == "chi_rx_rsp" and r["opcode"] == 4 and r["resp_err"] == 0
            assert r["node"] == origin["target"] and r["target"] == origin["node"] and r["txn_id"] == origin["txn_id"]
            assert (action is not None) == (kind == "cmo-evict-discard")
            if action: assert action["kind"] == 6
        elif kind in ("victim-writeback-discard", "cmo-writeback-commit"):
            assert origin["direction"] == "chi_tx_req" and origin["opcode"] in (23, 27, 66)
            assert r["direction"] == "chi_tx_dat" and r["opcode"] == 2
            assert r["node"] == origin["node"] and r["target"] == origin["target"]
            dbids = [e for e in events[origin_index+1:index] if e["handshake"] and
                     e["direction"] == "chi_rx_rsp" and e["node"] == origin["target"] and
                     e["target"] == origin["node"] and e["txn_id"] == origin["txn_id"] and e["opcode"] == 5]
            assert len(dbids) == 1 and r["txn_id"] == dbids[0]["dbid"]
            assert (action is not None) == (kind == "cmo-writeback-commit")
            if action: assert action["kind"] in (5, 6)
        else:
            raise AssertionError("unmodeled transition source: " + kind)
        if kind in INSTALL or kind in ("snoop-data-transient", "victim-writeback-discard", "cmo-writeback-commit"):
            # Every generated data source needs both accepted beats before its
            # transition, bound to this request rather than a numeric ID alone.
            beats = [e for e in events[origin_index+1:index+1] if e["handshake"] and
                     (e["direction"], e["node"], e["target"], e["txn_id"], e["opcode"]) ==
                     (r["direction"], r["node"], r["target"], r["txn_id"], r["opcode"])]
            ids = [e["data_id"] for e in beats]
            assert len(beats) == 2 and set(ids) == ({0, 1} if r["direction"].startswith("cchi") else {0, 2})
            payload = bytes.fromhex(t["payload"])
            assert len(payload) == 64
            for e in beats:
                beat = e["data_id"] if r["direction"].startswith("cchi") else e["data_id"] // 2
                assert bytes.fromhex(e["data"]) == payload[beat*32:(beat+1)*32] and e["resp_err"] == 0
            if kind in INSTALL: current[address] = t["payload"]
        if kind in DISCARD or kind == "makeunique-no-data": current[address] = None
        assert t["data"] == current[address], (address, kind, "version changed without an install/discard")
        last[address] = kind
    assert indices == sorted(set(indices)), "transition order differs from accepted canonical event order"
    assert dict(projection) == {t["address"]: t["transitions"] for t in architectural["l2Transitions"]}, "source coverage differs from transition ledger"
    assert {a: v for a, v in current.items() if v is not None} == {
        v["address"]: v["data"] for v in architectural["l2DataVersions"]}, "terminal versions disagree with transition replay"
    # Check only explicit generated terminal constraints. Absence of other
    # non-client residents remains legal; no replacement way becomes expected.
    geometry = directory[0]
    constrained = 0
    clients = {}
    for row in directory[1:]:
        for slice_id, entry in enumerate(row["entries"]):
            if not entry["state"]: continue
            address = ((entry["tag"] * geometry["sets"] + row["set"]) * 4 + slice_id) * 64
            if entry["client"]:
                key = entry["clientID"], address
                assert key not in clients
                clients[key] = entry["clientState"]
            kind = last.get(address)
            assert kind not in DISCARD, "discarded transition has a valid terminal Directory line"
            if kind == "upstream-writeback-install":
                assert entry["state"] == 3, "installed dirty WriteBack lost UD state"
                constrained += 1
            elif kind == "cmo-cleanshared-preserve":
                assert entry["state"] in (1, 2), "CleanShared left dirty Directory state"
                constrained += 1
            elif kind == "makeunique-no-data":
                assert entry["state"] == 2 and entry["client"] and entry["clientState"] == 2
                constrained += 1
    assert clients == {(r["node"], r["address"]): r["clientState"] for r in architectural["requesterFootprint"]}, "terminal client identity/permission differs from action replay"
    combinations = Counter(tuple(t["transitions"]) for t in architectural["l2Transitions"])
    return dict(transitionEvents=len(records), sourceCounts=dict(Counter(t["kind"] for t in records)),
                combinations=[dict(transitions=list(k), lines=v) for k, v in sorted(combinations.items())],
                terminalStateConstraints=constrained,
                scope="generated action/source identities, canonical transition order and bounded terminal Directory/DataStorage agreement; full residency remains open")


def mutations(events, actions, architectural, directory):
    records = architectural["l2TransitionEvents"]
    variants, covered = [], set()
    for index, record in enumerate(records):
        kind = record["kind"]
        if kind in covered: continue
        covered.add(kind)
        lost = copy.deepcopy(records); lost.pop(index); variants.append((lost, events, directory))
        for field, value in (("address", record["address"] + 64), ("kind", "unsupported-source")):
            changed = copy.deepcopy(records); changed[index][field] = value
            variants.append((changed, events, directory))
        if kind == "initial": continue
        for field, value in (("eventIndex", record["originIndex"]), ("originIndex", record["eventIndex"] + 1),
                             ("cycle", record["cycle"] + 1)):
            if value == record[field]: value = len(events)
            changed = copy.deepcopy(records); changed[index][field] = value
            variants.append((changed, events, directory))
        if record["actionIndex"] is not None:
            changed = copy.deepcopy(records); changed[index]["actionIndex"] = len(actions)
            variants.append((changed, events, directory))
        for field in ("address", "node", "txn_id"):
            changed_events = copy.deepcopy(events)
            changed_events[record["originIndex"]][field] += 64 if field == "address" else 1
            variants.append((records, changed_events, directory))
        if record["payload"] is not None:
            changed = copy.deepcopy(records)
            payload = bytearray.fromhex(record["payload"]); payload[-1] ^= 1
            changed[index]["payload"] = payload.hex(); variants.append((changed, events, directory))
            # Remove the final beat's acceptance: partial DAT cannot install or
            # satisfy a dirty copyback even when the final bytes look correct.
            changed_events = copy.deepcopy(events)
            changed_events[record["eventIndex"]]["handshake"] = False
            variants.append((records, changed_events, directory))
    noninitial = [i for i, t in enumerate(records) if t["eventIndex"] is not None]
    if len(noninitial) > 1:
        changed = copy.deepcopy(records); a, b = noninitial[:2]
        changed[a], changed[b] = changed[b], changed[a]; variants.append((changed, events, directory))
    last = {t["address"]: t["kind"] for t in records}
    geometry = directory[0]
    for address, kind in last.items():
        if kind in DISCARD:
            changed = copy.deepcopy(directory)
            line = address // 64
            set_id, slice_id = (line // 4) % geometry["sets"], line % 4
            slot = changed[1 + set_id * geometry["ways"]]["entries"][slice_id]
            slot.update(tag=line // (4 * geometry["sets"]), state=2, client=False)
            variants.append((records, events, changed))
    for row_index, row in enumerate(directory[1:], 1):
        for slice_id, entry in enumerate(row["entries"]):
            if not entry["state"]: continue
            address = ((entry["tag"] * geometry["sets"] + row["set"]) * 4 + slice_id) * 64
            kind = last.get(address)
            if kind in ("upstream-writeback-install", "cmo-cleanshared-preserve", "makeunique-no-data"):
                changed = copy.deepcopy(directory)
                changed[row_index]["entries"][slice_id]["state"] = 2 if kind == "upstream-writeback-install" else 3
                variants.append((records, events, changed))
    for changed, canonical, meta in variants:
        try: check(canonical, actions, architectural, meta, changed)
        except (AssertionError, KeyError, IndexError, TypeError, ValueError): pass
        else: raise AssertionError("transition audit accepted a mutated source/order/version")
    return dict(rejections=len(variants), sources=sorted(covered))
