"""Semantic checks on direct DUT resource samples; no expected cache data is derived here."""
from copy import deepcopy
import json
from pathlib import Path
import sys

FIELDS = ("mshrs", "responses", "writebacks", "requests", "queues", "pipeline",
          "serializers", "controllers", "retryOwners", "credits")


def check(rows):
    assert len(rows) >= 20, "resource trace cannot establish the terminal settle window"
    assert all(r["schema"] == 1 and type(r["ready"]) is bool for r in rows)
    assert all(r["sim_time"] == 2 * r["cycle"] for r in rows)
    assert all(b["cycle"] == a["cycle"] + 1 for a, b in zip(rows, rows[1:])), "resource samples have a gap"
    for row in rows:
        assert all(type(row[field]) is int and row[field] >= 0 for field in FIELDS)
    maximum = {field: max(r[field] for r in rows) for field in FIELDS}
    assert any(maximum.values()), "no DUT resource activity observed"
    assert all(r["ready"] and all(r[field] == 0 for field in FIELDS) for r in rows[-20:]), "live resource at success"
    return {"samples": len(rows), "peakOccupiedSlots": maximum, "terminalEmptyCycles": 20,
            "scope": "observed live context/queue/stage/serializer/controller/credit ownership; not cache residency or full architectural acceptance"}


def mutations(rows):
    check(rows)
    count = 0
    for field in FIELDS:
        copy = deepcopy(rows)
        copy[-1][field] = 1
        try:
            check(copy)
        except AssertionError:
            count += 1
        else:
            raise AssertionError("accepted a terminal resource leak: " + field)
    for kind in ("not_ready", "no_activity", "sample_gap"):
        copy = deepcopy(rows)
        if kind == "not_ready": copy[-1]["ready"] = False
        elif kind == "sample_gap": copy.pop(len(copy) // 2)
        else:
            for row in copy:
                for field in FIELDS: row[field] = 0
        try:
            check(copy)
        except AssertionError:
            count += 1
        else:
            raise AssertionError("accepted resource mutation: " + kind)
    return count


if __name__ == "__main__":
    rows = [json.loads(line) for line in (Path(sys.argv[1]) / "resources.jsonl").read_text().splitlines()]
    print(f"resource checker rejected {mutations(rows)} leaks/readiness/sampling invalid observations")
