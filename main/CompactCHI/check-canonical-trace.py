#!/usr/bin/env python3
"""Parse the host trace fixture using an independent JSON parser."""
import json
from pathlib import Path
import sys

prefix = Path(sys.argv[1])
rows = [json.loads(line) for line in Path(str(prefix) + ".jsonl").read_text().splitlines()]
assert rows[0] == {"schema": 1, "kind": "run", "seed": 42}
events = rows[1:]
assert len(events) == 15
assert [r["cycle"] for r in events] == [42, 43] + list(range(45, 58))
assert [r["sim_time"] for r in events[:2]] == [85, 87]
assert [r["handshake"] for r in events[:2]] == [False, True]
assert all(r["trace_tag"] is True for r in events[:2])
assert all(r["dbid"] == 9 and r["txn_id"] == 7 and r["address"] == 4096 for r in events[:2])
assert all(r["data"] == bytes(i ^ 0xa5 for i in range(32)).hex() for r in events[:2])
assert all(r["state_before"] == 0 and r["state_after"] == 2 and r["dirty"] is False for r in events[:2])
assert all(r["allow_retry"] is False and r["exp_comp_ack"] is True and r["p_crd_type"] == 3 for r in events[:2])
assert all(r["size"] == 6 and r["mem_attr"] == 13 and r["order"] == 0 and r["ns"] is False and r["likely_shared"] is True for r in events[:2])
assert len({r["direction"] for r in events[2:]}) == 13
for row in events[2:]:
    assert all(row[key] is None for key in ("sim_time", "address", "dbid", "data", "state_before", "state_after", "allow_retry", "exp_comp_ack", "p_crd_type", "size", "mem_attr", "order", "ns", "likely_shared"))
    assert row["handshake"] is True
text = Path(str(prefix) + ".log").read_text()
assert "cycle=42" in text and "sim_time=85" in text and "TraceTag=1" in text
print("CanonicalTrace JSONL/text: timestamps, TraceTag, data and unknown-field semantics passed")
