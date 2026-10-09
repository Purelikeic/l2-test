#!/usr/bin/env python3
"""Validate the host-only shared Sequencer fixture's actual cycle trace."""
import json
from pathlib import Path
import sys

records = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
assert records[0] == {"schema": 1, "kind": "run", "seed": 17}
request, first, last = records[1:]
assert request["direction"] == "cchi_rx_req" and request["opcode"] == 3
assert [first["direction"], last["direction"]] == ["cchi_tx_dat", "cchi_tx_dat"]
assert request["cycle"] >= 10
assert first["cycle"] >= request["cycle"] + 4
assert last["cycle"] == first["cycle"] + 1
assert all(row["txn_id"] == 15 and row["address"] == 4096 and row["handshake"] is True for row in records[1:])
assert all(row["trace_tag"] is True and row["sim_time"] is None for row in records[1:])
assert first["data_id"] == 0 and last["data_id"] == 1
assert first["dbid"] == last["dbid"] == 0
assert bytes.fromhex(first["data"] + last["data"]) == bytes(i ^ 17 for i in range(64))
print(f"Shared Sequencer host trace: seed=17 txn=15 response_latency={last['cycle'] - request['cycle']} cycles passed")
