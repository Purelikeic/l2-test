"""Independent forward endpoint evidence checker; bounded lower-Snoop topology only."""
import copy
import json
from pathlib import Path


def check(root, events, profile):
    path = root / "forward.jsonl"
    rows = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    dct = [r for r in events if r["handshake"] and r["direction"] == "chi_tx_dat" and r["target"] == 12 and r["opcode"] == 4]
    if profile != "lower-snoop":
        assert not rows and not dct, "non-lower profile created a forward endpoint obligation"
        return dict(observations=0, dctBeats=0, delayedAcks=0, mutationRejections=0,
                    scope="no external forward obligation in this profile")
    assert len(dct) == 2 and len(rows) == 3
    dct = sorted(dct, key=lambda row: row["cycle"])
    for observation, event in zip(rows[:2], dct):
        assert observation["kind"] == "data"
        assert observation["cycle"] == event["cycle"]
        assert observation["txn"] == event["txn_id"]
        assert observation["dbid"] == event["dbid"]
        assert observation["source"] == event["node"] == 1
        assert observation["target"] == event["target"] == 12
        assert observation["beat"] == event["data_id"] // 2
        assert observation["trace_tag"] == event["trace_tag"]
    ack = rows[2]
    assert ack["kind"] == "ack"
    assert ack["cycle"] == dct[-1]["cycle"] + 1
    assert ack["txn"] == dct[0]["dbid"] == ack["dbid"]
    assert ack["source"] == 12 and ack["target"] == 8
    assert ack["trace_tag"] == dct[0]["trace_tag"]
    return dict(observations=len(rows), dctBeats=len(dct), delayedAcks=1,
                cycle=ack["cycle"], forwardTxn=dct[0]["txn_id"], dbid=dct[0]["dbid"],
                scope="independent DCT collection and next-cycle CompAck; fixed bounded forward endpoint")


def mutations(root, events, profile):
    if profile != "lower-snoop": return 0
    original = [json.loads(line) for line in (root / "forward.jsonl").read_text().splitlines()]
    rejected = 0
    variants = []
    for index, field, value in ((0, "cycle", original[0]["cycle"] + 1),
                                (1, "txn", original[1]["txn"] + 1),
                                (2, "txn", original[2]["txn"] + 1),
                                (2, "dbid", original[2]["dbid"] + 1),
                                (2, "target", 12),
                                (2, "trace_tag", not original[2]["trace_tag"]),
                                (2, "kind", "data")):
        changed = copy.deepcopy(original)
        changed[index][field] = value
        variants.append(changed)
    for changed in variants:
        path = root / "forward.jsonl"
        backup = path.read_text()
        path.write_text("".join(json.dumps(row) + "\n" for row in changed))
        try: check(root, events, profile)
        except (AssertionError, KeyError): rejected += 1
        else: raise AssertionError("forward checker accepted a mutated endpoint observation")
        finally: path.write_text(backup)
    return rejected
