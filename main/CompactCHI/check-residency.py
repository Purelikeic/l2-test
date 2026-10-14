"""Bounded terminal membership from validated intent/protocol obligations.

No observed Directory tag/way or replacement choice supplies golden membership.
The no-pressure proof uses fixed address mapping and verified invalid-way-first
allocation. Overflow needs an independently validated fixed victim vector.
"""
import copy
from collections import defaultdict, deque

DEFAULT_GEOMETRY = dict(schema=1, sets=512, ways=8, slices=4, lineBytes=64, addrBits=48)
ALLOCATE = {"refill", "makeunique-no-data"}
DISCARD = {"cmo-evict-discard", "cmo-cleaninvalid-discard", "cmo-makeinvalid-discard"}
VICTIM = {"victim-evict-discard", "victim-writeback-discard"}


def derive(events, actions, architectural, geometry, profile="host", base=None):
    assert geometry["slices"] == 4 and geometry["lineBytes"] == 64
    sets, ways = geometry["sets"], geometry["ways"]
    def group(address): return address // 64 % 4, address // 256 % sets
    allocated = defaultdict(set)
    for t in architectural["l2TransitionEvents"]:
        if t["kind"] in ALLOCATE: allocated[group(t["address"])].add(t["address"])
    scoped = {(s,t) for s in range(4) for t in range(sets) if len(allocated[s,t]) <= ways}
    fixed_group, fixed_addresses, fixed_victims = None, set(), set()
    if profile == "victim":
        assert geometry == DEFAULT_GEOMETRY and base is not None
        stride = geometry["slices"] * geometry["sets"] * geometry["lineBytes"]
        addresses = [base + i * stride for i in range(12)]
        fixed_group = group(base)
        assert all(group(a) == fixed_group for a in addresses)
        assert allocated[fixed_group] == set(addresses), "fixed victim vector has extra/missing allocations"
        # Caller must first validate check-victim-events.py's independent
        # requests/read/victim vector, including the dirty commitment/readback.
        fixed_addresses = {addresses[i] for i in (0,5,6,7,8,9,10,11)}
        fixed_victims = set(addresses[:5]); scoped.add(fixed_group)
    by_index = defaultdict(list)
    for t in architectural["l2TransitionEvents"]:
        if t["eventIndex"] is not None: by_index[t["eventIndex"]].append(t)
    state, lower, cache_requests, ack_owners = {}, {}, {}, {}
    intents = defaultdict(deque)
    for a in actions:
        if a["kind"] != 11: intents[a["node"],a["txn"]].append(a)
    exclusions, effects, cleaned = {}, defaultdict(set), set()
    for index, r in enumerate(events):
        if not r["handshake"]: continue
        direction, op = r["direction"], r["opcode"]
        if direction in ("cchi_rx_req", "cchi_rx_evt"):
            action = intents[r["node"],r["txn_id"]].popleft()
            if action["kind"] in (0,1,8):
                address = action["address"]
                if any(s["address"] == address for s in lower.values()):
                    exclusions[group(address)] = "overlapping lower Snoop and requester action"
                cache_requests[r["node"],r["txn_id"]] = dict(action=action, beats=set(), granted=False)
        for t in by_index[index]:
            address, kind = t["address"], t["kind"]
            before = state.get(address,0)
            if kind in ALLOCATE | DISCARD | VICTIM | {"upstream-writeback-install", "snoop-data", "cmo-cleanshared-preserve"}:
                effects[r["cycle"], address].add(kind)
            if kind == "refill":
                origin = events[t["originIndex"]]
                state[address] = 3 if r["resp"] & 4 else 2 if origin["opcode"] == 7 else 1
                cleaned.discard(address)
            elif kind == "makeunique-no-data":
                state[address] = 2; cleaned.discard(address)
            elif kind == "upstream-writeback-install":
                state[address] = 3; cleaned.discard(address)
            elif kind == "snoop-data":
                # The supported retained probe-data copy uses the documented
                # nested C data path: cSetDirty, while client permission stays separate.
                state[address] = 3; cleaned.discard(address)
            elif kind in DISCARD: state[address] = 0
            elif kind in VICTIM:
                g = group(address)
                if g == fixed_group:
                    assert address in fixed_victims, "replacement differs from independent fixed vector"
                    state[address] = 0
                elif g in scoped:
                    raise AssertionError("capacity victim occurred in a no-pressure group")
                # Other overflow victims are NOT promoted to golden absence.
            elif kind == "cmo-cleanshared-preserve" and before:
                assert before != 3 or address in cleaned, "dirty CleanShared lacks its independently validated writeback commitment"
                state[address] = 1 if before == 1 else 2
            elif kind == "cmo-writeback-commit": cleaned.add(address)
            elif kind in ("evict-client-release", "snoop-no-data", "snoop-data-transient"):
                pass
        if direction == "cchi_tx_dat" and (r["target"],r["txn_id"]) in cache_requests:
            key = r["target"],r["txn_id"]; q = cache_requests[key]
            q["beats"].add(r["data_id"])
            if q["beats"] == {0,1}:
                address = q["action"]["address"]
                if group(address) in scoped and state.get(address,0) == 0:
                    exclusions[group(address)] = "grant has no uniquely ordered allocation"
                if q["action"]["kind"] in (0,1): effects[r["cycle"],address].add("requester-grant")
                if q["action"]["kind"] == 0 and state.get(address,0):
                    state[address] = 3 if state[address] == 3 else 1
                elif q["action"]["kind"] == 1 and state.get(address,0):
                    state[address] = 3 if state[address] == 3 else 2
                q["granted"] = True
                if q["action"]["kind"] == 8: del cache_requests[key]
                else: ack_owners[r["target"],r["dbid"]] = key
        elif direction == "cchi_rx_rsp" and op == 0 and (r["node"],r["txn_id"]) in ack_owners:
            key = ack_owners.pop((r["node"],r["txn_id"])); del cache_requests[key]
        elif direction == "chi_rx_snp":
            address = r["address"]
            if any(q["action"]["address"] == address for q in cache_requests.values()):
                exclusions[group(address)] = "overlapping requester and lower Snoop"
            assert op in (1,3,7,9,10,16,17,23), "unmodeled lower Snoop residency"
            lower[r["node"],r["txn_id"]] = dict(address=address, opcode=op, beats=set())
        elif direction in ("chi_tx_rsp","chi_tx_dat") and (r["target"],r["txn_id"]) in lower:
            key = r["target"],r["txn_id"]; q = lower[key]
            done = direction == "chi_tx_rsp"
            if direction == "chi_tx_dat" and op in (1,6):
                q["beats"].add(r["data_id"]); done = q["beats"] == {0,2}
            if done:
                address = q["address"]
                if q["opcode"] not in (3,16): effects[r["cycle"],address].add("lower-snoop-final")
                if q["opcode"] in (7,9,10,23): state[address] = 0
                elif q["opcode"] in (1,17) and state.get(address,0): state[address] = 1
                # Once and Query retain the previous installed state/version.
                del lower[key]
    assert not cache_requests and not ack_owners and not lower
    for (cycle,address), kinds in effects.items():
        if len(kinds) > 1:
            exclusions[group(address)] = "same-cycle residency effects lack an independent order"
    scoped -= set(exclusions)
    if fixed_group is not None:
        actual_model = {a for a,s in state.items() if s and group(a) == fixed_group}
        assert actual_model == fixed_addresses, "transition replay and independent survivor vector disagree"
    clients = {r["address"] for r in architectural["requesterFootprint"]}
    versions = {r["address"]:r["data"] for r in architectural["l2DataVersions"]}
    undefined = set(architectural["l2UndefinedStorage"])
    expected = []
    for address, s in sorted(state.items()):
        if s and group(address) in scoped:
            assert address in versions or address in undefined, "resident model lacks an independent version"
            expected.append(dict(address=address,state=s,client=address in clients,
                                 data=versions.get(address),undefined=address in undefined))
    return dict(geometry=geometry, groups=[dict(slice=s,set=t) for s,t in sorted(scoped)],
                expected=expected, allocated=[dict(slice=g[0],set=g[1],addresses=sorted(a)) for g,a in sorted(allocated.items())],
                exclusions=[dict(slice=g[0],set=g[1],reason=exclusions.get(g,"overflow lacks independent victim vector"))
                            for g in sorted((set(allocated)-scoped) | set(exclusions))],
                policy="fixed geometry, no way hints/flush/errors, validated allocations and invalid-way-first; fixed-vector exception only",
                scope="bounded membership and non-client state/data; arbitrary replacement and overlap remain excluded")


def check(directory, data, model):
    assert directory[0] == data[0] == model["geometry"]
    geometry = model["geometry"]; scoped = {(g["slice"],g["set"]) for g in model["groups"]}
    assert len(directory) == len(data) == geometry["sets"]*geometry["ways"]+1
    expected = {r["address"]:r for r in model["expected"]}
    assert len(expected) == len(model["expected"])
    observed, nonclient = set(), 0
    for index,(meta, sample) in enumerate(zip(directory[1:],data[1:])):
        assert (meta["set"],meta["way"]) == (sample["set"],sample["way"]) == divmod(index,geometry["ways"])
        assert len(meta["entries"]) == len(sample["lines"]) == 4
        for slice_id, entry in enumerate(meta["entries"]):
            if not entry["state"] or (slice_id,meta["set"]) not in scoped: continue
            address = ((entry["tag"]*geometry["sets"]+meta["set"])*4+slice_id)*64
            assert address in expected, "unexpected valid tag in a proved residency group"
            assert address not in observed; observed.add(address)
            wanted = expected[address]
            assert entry["client"] == wanted["client"], "non-client residency became a client"
            if not wanted["client"]:
                assert entry["state"] == wanted["state"], "non-client state differs from independent transition obligations"
                assert not wanted["undefined"] and wanted["data"] is not None
                assert sample["lines"][slice_id]["data"] == wanted["data"]
                assert not sample["lines"][slice_id]["error"]
                nonclient += 1
    assert observed == set(expected), "required resident tag is missing"
    return dict(provedGroups=len(scoped), residentLines=len(observed), nonClientLines=nonclient,
                excludedGroups=len(model["exclusions"]), expected=model["expected"], exclusions=model["exclusions"],
                scope=model["scope"])


def mutations(directory, data, model):
    variants = []
    covered = set()
    geometry=model["geometry"]; scoped={(g["slice"],g["set"]) for g in model["groups"]}
    for i,row in enumerate(directory[1:],1):
        for s,e in enumerate(row["entries"]):
            if not e["state"] or (s,row["set"]) not in scoped: continue
            # Positive check() still inspects every slot/byte. Negative audit
            # probes need one representative per semantic class, not a full
            # Directory clone for every resident in a long pressure stream.
            signature = (s, e["state"], e["client"])
            if signature in covered: continue
            covered.add(signature)
            for field,value in (("state",0),("tag",e["tag"]+100),("client",not e["client"])):
                changed=copy.deepcopy(directory); changed[i]["entries"][s][field]=value
                variants.append((changed,data))
            if not e["client"]:
                changed=copy.deepcopy(directory); changed[i]["entries"][s]["state"]=1 if e["state"]!=1 else 3
                variants.append((changed,data))
                changed=copy.deepcopy(data)
                payload=bytearray.fromhex(changed[i]["lines"][s]["data"]); payload[0]^=1
                changed[i]["lines"][s]["data"]=payload.hex(); variants.append((directory,changed))
    expected_addresses={r["address"] for r in model["expected"]}
    for i,row in enumerate(directory[1:],1):
        added=False
        for s,e in enumerate(row["entries"]):
            if e["state"] or (s,row["set"]) not in scoped: continue
            tag=0
            while ((tag*geometry["sets"]+row["set"])*4+s)*64 in expected_addresses: tag+=1
            changed=copy.deepcopy(directory); changed[i]["entries"][s].update(tag=tag,state=1,client=False)
            variants.append((changed,data)); added=True; break
        if added: break
    for changed,samples in variants:
        try: check(changed,samples,model)
        except (AssertionError,KeyError,ValueError): pass
        else: raise AssertionError("residency checker accepted a missing/extra/state/client/data mutation")
    return len(variants)
