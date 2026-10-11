"""Terminal DataStorage versions from independent intent/Home replay, never DUT bytes."""
import copy
from pathlib import Path
import runpy

FIELDS = runpy.run_path(str(Path(__file__).with_name("check-resources.py")))["FIELDS"]


def check(directory_rows, rows, resources, versions, undefined=(), transitions=()):
    geometry, *directory = directory_rows
    data_geometry, *samples = rows
    assert geometry == data_geometry
    sets, ways, slices, line = (geometry[k] for k in ("sets", "ways", "slices", "lineBytes"))
    assert len(directory) == len(samples) == sets * ways and slices == 4 and line == 64
    assert samples[0]["cycle"] == directory[-1]["cycle"] + 1
    assert samples[-1]["cycle"] == resources[-1]["cycle"]
    by_cycle = {r["cycle"]: r for r in resources}
    assert len(by_cycle) == len(resources)
    # Keep the entire Directory/DataStorage snapshot window stable and drained.
    for cycle in range(directory[0]["cycle"] - 20, samples[-1]["cycle"] + 1):
        r = by_cycle[cycle]
        assert r["ready"] and all(r[field] == 0 for field in FIELDS)
    known = {r["address"]: bytes.fromhex(r["data"]) for r in versions}
    assert len(known) == len(versions) and all(len(data) == line for data in known.values())
    transition_map = {r["address"]: r["transitions"] for r in transitions}
    allowed = {"initial", "refill", "snoop-data", "snoop-no-data", "makeunique-no-data", "upstream-writeback-install", "evict-discard", "writeback-discard", "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard", "cmo-cleanshared-preserve"}
    for address in known:
        assert address in transition_map and any(kind in transition_map[address] for kind in ("refill", "snoop-data", "upstream-writeback-install")), (address, "known version lacks a transition ledger")
    for address in undefined:
        assert address in transition_map, (address, "undefined version lacks a transition ledger")
    for address, sequence in transition_map.items():
        assert sequence and sequence[0] == "initial" and set(sequence) <= allowed, (address, sequence)
        stores = [i for i, kind in enumerate(sequence) if kind in ("refill", "snoop-data", "upstream-writeback-install")]
        discards = [i for i, kind in enumerate(sequence) if kind in ("evict-discard", "writeback-discard", "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard")]
        if address in known:
            assert stores, (address, sequence)
        if address in undefined:
            assert not stores and "makeunique-no-data" in sequence and (not discards or sequence[-1] == "cmo-or-eviction-discard"), (address, sequence)
    checked, unknown, invalid = 0, [], 0
    valid_addresses = set()
    for index, (meta, data) in enumerate(zip(directory, samples)):
        assert (meta["set"], meta["way"]) == (data["set"], data["way"]) == divmod(index, ways)
        assert data["cycle"] == samples[0]["cycle"] + index
        assert data["sim_time"] == data["cycle"] * 2 == by_cycle[data["cycle"]]["sim_time"]
        assert len(data["lines"]) == slices
        for slice_id, entry in enumerate(meta["entries"]):
            observed = data["lines"][slice_id]
            assert type(observed["error"]) is bool and not observed["error"]
            actual = bytes.fromhex(observed["data"])
            assert len(actual) == line
            if entry["state"] == 0:
                invalid += 1
                continue # Invalid SRAM payload is not architectural state.
            address = ((entry["tag"] * sets + meta["set"]) * slices + slice_id) * line
            valid_addresses.add(address)
            if address not in known:
                assert address in undefined, (address, "resident has no independently expected data version")
                assert entry["client"] and entry["clientState"] == 2, "undefined MakeUnique data lacks a Unique client"
                unknown.append(address)
                continue # MakeUnique can allocate permission with no data installation.
            assert actual == known[address], (address, "DataStorage differs from independent expected version")
            checked += 1
    for address, sequence in transition_map.items():
        discards = [kind for kind in sequence if kind in ("evict-discard", "writeback-discard", "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard")]
        if address in valid_addresses and address in known:
            assert not discards or max(i for i, kind in enumerate(sequence) if kind in ("refill", "snoop-data", "upstream-writeback-install")) > max(i for i, kind in enumerate(sequence) if kind in ("evict-discard", "writeback-discard", "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard")), (address, sequence)
        if discards and sequence[-1] in ("evict-discard", "writeback-discard", "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard"):
            assert address not in valid_addresses, (address, sequence, "discarded line remains valid")
    return dict(scanCycles=len(samples), observedSlots=len(samples) * slices, checkedLines=checked,
                unknownValidLines=len(unknown), unknownAddresses=unknown, invalidSlots=invalid,
                scope="terminal bytes of independently modeled refill/retained-Snoop/WriteBack versions; undefined MakeUnique storage and full residency remain open")


def mutations(directory, rows, resources, versions, undefined=(), transitions=()):
    variants = []
    for mutate in (
        lambda r: r.pop(),
        lambda r: r.append(copy.deepcopy(r[-1])),
        lambda r: r[1].update(set=r[0]["sets"]),
        lambda r: r[2].update(set=r[1]["set"], way=r[1]["way"]),
        lambda r: r[1].update(cycle=r[1]["cycle"] + 1),
        lambda r: r[1].update(sim_time=r[1]["sim_time"] + 1),
        lambda r: r[1]["lines"].pop(),
        lambda r: r[1]["lines"][0].update(error=True),
        lambda r: r[1]["lines"][0].update(data="00"),
    ):
        changed = copy.deepcopy(rows); mutate(changed); variants.append((changed, resources))
    for field in (*FIELDS, "ready"):
        changed = copy.deepcopy(resources)
        next(r for r in changed if r["cycle"] == rows[1]["cycle"])[field] = False if field == "ready" else 1
        variants.append((rows, changed))
    known = {v["address"] for v in versions}
    for i, meta in enumerate(directory[1:], 1):
        for slice_id, entry in enumerate(meta["entries"]):
            address = ((entry["tag"] * directory[0]["sets"] + meta["set"]) * 4 + slice_id) * 64
            if entry["state"] and address in known:
                for byte in (0, 31, 32, 63):
                    changed = copy.deepcopy(rows)
                    actual = bytearray.fromhex(changed[i]["lines"][slice_id]["data"])
                    actual[byte] ^= 1; changed[i]["lines"][slice_id]["data"] = actual.hex()
                    variants.append((changed, resources))
                break
        else: continue
        break
    for changed, resource in variants:
        try: check(directory, changed, resource, versions, undefined, transitions)
        except (AssertionError, KeyError, ValueError): pass
        else: raise AssertionError("DataStorage checker accepted a mutated line/scan/drain")
    if transitions:
        broken = copy.deepcopy(transitions)
        for entry in broken:
            entry["transitions"] = [kind for kind in entry["transitions"] if kind not in ("refill", "snoop-data", "upstream-writeback-install", "writeback")]
            if entry["address"] in {v["address"] for v in versions}:
                try: check(directory, rows, resources, versions, undefined, broken)
                except (AssertionError, KeyError, ValueError): pass
                else: raise AssertionError("DataStorage checker accepted a transition-ledger mutation")
                break
    return len(variants)
