"""Qualify bounded random action segments from INI and independent action intent."""
import configparser
from collections import Counter


def check(root, actions, resources):
    by_cycle = {r["cycle"]: r for r in resources}
    fields = ("mshrs", "responses", "writebacks", "requests", "queues", "pipeline",
              "serializers", "controllers", "retryOwners", "credits")
    for action in actions:
        sample = by_cycle[action["cycle"]]
        assert sample["ready"] and all(sample[field] == 0 for field in fields), "random action began with live resources"
    ini = configparser.ConfigParser()
    ini.read(root / "cchitest.ini")
    cfg, fuzz = ini["tltest.config"], ini["tltest.fuzzer"]
    episodes, lines = int(fuzz.get("random.episodes", "32"), 0), int(fuzz.get("random.lines", "4"), 0)
    assert 1 <= episodes <= 40 and 1 <= lines <= 8
    memory, device = int(cfg["memory.start"], 0), int(cfg["mmio.start"], 0)
    index = segments = 0
    kinds = Counter()
    while index < len(actions):
        first = actions[index]
        k = first["kind"]
        if k == 8: expected = [8]
        elif k == 0: expected = [0, 5, 3]
        elif k == 1:
            assert index + 2 < len(actions), "incomplete acquire/store/release segment"
            release = actions[index+2]["kind"]
            assert release in (4, 5, 6, 7)
            expected = [1, 11, release] + ([3] if release == 5 else [])
        elif k == 10: expected = [10, 9]
        else: raise AssertionError("unsupported random segment start")
        rows = actions[index:index+len(expected)]
        assert len(rows) == len(expected) and [a["kind"] for a in rows] == expected
        assert all(a["node"] == first["node"] and a["address"] == first["address"] for a in rows), "segment changed its client/line"
        start = device if k == 10 else memory
        assert start <= first["address"] < start + lines*64 and first["address"] % 64 == 0
        for a in rows:
            assert a["txn"] == 20 + index and a["trace_tag"] == (index % 2 == 0)
            assert a["size"] == (3 if k == 10 else 6)
            if a["kind"] == 10: assert 0 < a["byte_enable"] <= 255
            kinds[a["kind"]] += 1
            index += 1
        segments += 1
    assert segments == episodes, "random action budget was not completed"
    return dict(segments=segments, acceptedActions=len(actions), workingLines=lines,
                actionKinds=dict(sorted(kinds.items())),
                scope="bounded seeded action segments with released clients; no arbitrary overlap/global acceptance")
