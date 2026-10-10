"""Host acceptance for DataStorage's version/scan boundary, separate from native evidence."""
import copy
import runpy
from pathlib import Path

checkers = runpy.run_path(str(Path(__file__).with_name("check-datastorage.py")))
check, mutations, fields = checkers["check"], checkers["mutations"], checkers["FIELDS"]
g = dict(schema=1, sets=2, ways=2, slices=4, lineBytes=64, addrBits=48)
old = bytes((i * 17 ^ 42) & 255 for i in range(64))
new = bytearray(old); new[0] ^= 0xa5; new[63] ^= 0x5a; new = bytes(new)
directory, data = [g], [g]
for i in range(4):
    entries = [dict(tag=0, state=0, client=False, clientState=0, clientID=0) for s in range(4)]
    lines = [dict(data=bytes(64).hex(), error=False) for s in range(4)]
    if i == 0:
        entries[0].update(tag=8, state=2, client=True, clientState=2)
        lines[0]["data"] = old.hex() # L1 local Store does not update the stored L2 copy.
        entries[1].update(tag=9, state=3) # Installed dirty release has latest data.
        lines[1]["data"] = new.hex()
        entries[2].update(tag=10, state=2, client=True, clientState=2, clientID=2)
        lines[2]["data"] = "ab" * 64 # Explicit no-data MakeUnique allocation.
    directory.append(dict(cycle=30+i, sim_time=(30+i)*2, set=i//2, way=i%2, entries=entries))
    data.append(dict(cycle=34+i, sim_time=(34+i)*2, set=i//2, way=i%2, lines=lines))
resources = [dict(cycle=c, sim_time=c*2, ready=True, **{f:0 for f in fields}) for c in range(10,38)]
versions = [dict(address=4096, data=old.hex()), dict(address=4672, data=new.hex())]
undefined = [5248]
transitions = [dict(address=4096, transitions=["initial", "refill"]), dict(address=4672, transitions=["initial", "snoop-data"]), dict(address=5248, transitions=["initial", "makeunique-no-data"])]
result = check(directory,data,resources,versions,undefined,transitions)
assert result["checkedLines"] == 2 and result["unknownValidLines"] == 1
rejected = mutations(directory,data,resources,versions,undefined,transitions)
for variant in ("latest-overwrites-stale", "drop-model", "undefined-without-client", "invent-unknown-resident"):
    metas, rows, golden = copy.deepcopy(directory), copy.deepcopy(data), copy.deepcopy(versions)
    if variant == "latest-overwrites-stale": rows[1]["lines"][0]["data"] = new.hex()
    elif variant == "drop-model": golden.pop()
    elif variant == "undefined-without-client": metas[1]["entries"][2]["client"] = False
    else: metas[2]["entries"][0].update(state=2, tag=100)
    try: check(metas,rows,resources,golden,undefined,transitions)
    except AssertionError: rejected += 1
    else: raise AssertionError("DataStorage accepted " + variant)
for bad in (
    [dict(address=4096, transitions=["initial", "writeback"]) , *transitions[1:]],
    [dict(address=4096, transitions=["initial", "cmo-or-eviction-discard"]), *transitions[1:]],
    [dict(address=4096, transitions=["refill"]), *transitions[1:]],
    [transitions[0], transitions[1], dict(address=5248, transitions=["initial", "refill"])]):
    try: check(directory,data,resources,versions,undefined,bad)
    except AssertionError: rejected += 1
    else: raise AssertionError("DataStorage accepted an invalid transition sequence")
print("DataStorage independent host checker: stale/latest/no-data versions, transition order, 16 slots and", rejected, "mutations passed")
