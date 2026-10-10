"""Independent terminal Directory client inclusion, not full L2 residency/data."""
import copy
import json
from pathlib import Path
import runpy

FIELDS = runpy.run_path(str(Path(__file__).with_name("check-resources.py")))["FIELDS"]


def check(rows, resources, expected, roles):
    geometry, *samples = rows
    sets, ways, slices, line, bits = (geometry[k] for k in ("sets", "ways", "slices", "lineBytes", "addrBits"))
    assert geometry["schema"] == 1 and sets > 0 and ways > 0
    assert not sets & (sets - 1) and not ways & (ways - 1)
    assert slices == 4 and line == 64 and 8 < bits < 64
    assert sets * slices * line < 1 << bits
    assert len(samples) == sets * ways, "incomplete/extra Directory sweep"
    start = samples[0]["cycle"]
    by_cycle = {r["cycle"]: r for r in resources}
    assert len(by_cycle) == len(resources)
    # Observe a stable drained state for twenty cycles before and throughout
    # the whole sweep; port quietness alone never makes a snapshot admissible.
    for cycle in range(start - 20, start + len(samples)):
        r = by_cycle[cycle]
        assert r["ready"] and all(r[k] == 0 for k in FIELDS), "Directory sweep preceded/lost resource drain"
    assert samples[-1]["cycle"] == resources[-1]["cycle"], "Directory sweep is not terminal"
    resident, clients = set(), {}
    for index, row in enumerate(samples):
        assert row["cycle"] == start + index and row["sim_time"] == row["cycle"] * 2
        assert (row["set"], row["way"]) == divmod(index, ways), "skipped/duplicate Directory coordinate"
        assert len(row["entries"]) == slices
        assert row["sim_time"] == by_cycle[row["cycle"]]["sim_time"]
        for slice_id, e in enumerate(row["entries"]):
            assert 0 <= e["tag"] < (1 << bits) // (sets * slices * line)
            assert e["state"] in (0, 1, 2, 3) and type(e["client"]) is bool
            address = ((e["tag"] * sets + row["set"]) * slices + slice_id) * line
            if e["state"]:
                assert address not in resident, "duplicated valid Directory tag"
                resident.add(address)
            if e["client"]:
                assert e["state"] != 0 and e["clientState"] in (1, 2)
                assert roles[e["clientID"]] == 0, "Directory client is transient/MMIO"
                key = e["clientID"], address
                assert key not in clients
                clients[key] = e["clientState"]
    assert clients == expected, "Directory client identities/permissions differ from independent requester replay"
    return dict(scanCycles=len(samples), observedSlots=len(samples) * slices,
                observedResidentLines=len(resident), clientLines=len(clients),
                scope="terminal Directory inclusion and client identity/Shared-Unique permission; complete L2 residency/data remains open")


def mutations(rows, resources, expected, roles):
    variants = []
    for mutate in (
        lambda r: r.pop(),
        lambda r: r.append(copy.deepcopy(r[-1])),
        lambda r: r[2].update(set=r[1]["set"], way=r[1]["way"]),
        lambda r: r[2].update(cycle=r[2]["cycle"] + 1),
        lambda r: r[1].update(sim_time=r[1]["sim_time"] + 1),
        lambda r: r[1]["entries"].pop(),
        lambda r: r[1]["entries"][0].update(state=0, client=True, clientState=2, clientID=0),
        lambda r: r[1]["entries"][0].update(state=2, client=True, clientState=0, clientID=0),
        lambda r: r[1]["entries"][0].update(state=2, client=True, clientState=2, clientID=max(roles) + 1),
        lambda r: r[1]["entries"][0].update(state=2, client=True, clientState=2, clientID=next(n for n in roles if roles[n] != 0)),
        lambda r: r[1]["entries"][0].update(tag=1 << r[0]["addrBits"]),
    ):
        changed = copy.deepcopy(rows); mutate(changed); variants.append((changed, resources))
    # Force a duplicate valid tag in two ways of the same set and Slice.
    if rows[0]["ways"] > 1:
        changed = copy.deepcopy(rows)
        for index in (1, 2): changed[index]["entries"][0].update(tag=0, state=2, client=False)
        variants.append((changed, resources))
    for field in FIELDS:
        changed = copy.deepcopy(resources)
        next(r for r in changed if r["cycle"] == rows[1]["cycle"])[field] = 1
        variants.append((rows, changed))
    changed = copy.deepcopy(resources)
    next(r for r in changed if r["cycle"] == rows[1]["cycle"])["ready"] = False
    variants.append((rows, changed))
    clients = [(i, s) for i, row in enumerate(rows[1:], 1) for s, e in enumerate(row["entries"]) if e["client"]]
    if clients:
        index, slice_id = clients[0]
        for field, value in (("client", False), ("clientState", 3 - rows[index]["entries"][slice_id]["clientState"]),
                             ("clientID", max(roles) + 1), ("tag", rows[index]["entries"][slice_id]["tag"] ^ 1)):
            changed = copy.deepcopy(rows); changed[index]["entries"][slice_id][field] = value
            variants.append((changed, resources))
    else:
        changed = copy.deepcopy(rows)
        changed[1]["entries"][0].update(tag=0, state=2, client=True, clientState=2, clientID=next(n for n in roles if roles[n] == 0) if 0 in roles.values() else 0)
        variants.append((changed, resources))
    for changed, resource in variants:
        try: check(changed, resource, expected, roles)
        except (AssertionError, KeyError): pass
        else: raise AssertionError("Directory checker accepted a mutated state/scan/drain")
    return len(variants)


if __name__ == "__main__":
    import sys
    rows = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
    resources = [dict(cycle=c, sim_time=c * 2, ready=True, **{k: 0 for k in FIELDS}) for c in range(10, 34)]
    expected = {(0, 4096): 2, (2, 4672): 1}
    roles = {0: 0, 1: 1, 2: 0, 3: 2}
    evidence = check(rows, resources, expected, roles)
    evidence["mutationRejections"] = mutations(rows, resources, expected, roles)
    print("Directory independent checker:", evidence)
