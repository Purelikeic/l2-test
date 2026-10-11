"""Bounded independent terminal Requester/Home replay; no internal L2 residency oracle."""
import copy
import json
from collections import defaultdict, deque


def check(root, events, seed, topo, terminal=None):
    terminal = terminal if terminal is not None else json.loads((root / "architectural-state.json").read_text())
    assert terminal["schema"] == 1
    actions = [json.loads(line) for line in (root / "actions.jsonl").read_text().splitlines()]
    latest, memory, devices, copies = {}, {}, {}, {}
    l2_data, down_reads, undefined_storage = {}, {}, set()
    transitions = defaultdict(list)
    requests, snoops, acks, writes, write_dbids = {}, {}, {}, {}, {}
    roles = {n["id"]: n["kind"] for n in topo["nodes"]}

    def initialize(address):
        address &= ~63
        if address not in latest:
            latest[address] = bytes(((address >> 6) ^ (i * 17) ^ seed) & 255 for i in range(64))
            memory[address] = latest[address]
            transitions[address].append("initial")
        return address

    # prepare() initializes cache lines even when an API attempt is Deferred.
    # These addresses are stimulus inputs, not terminal state observations.
    for line in (root / "rejections.jsonl").read_text().splitlines():
        attempt = json.loads(line)
        if attempt["kind"] not in (9, 10): initialize(attempt["address"])

    def retire(key):
        del requests[key]

    def snoop_finish(key, resp):
        s = snoops.pop(key)
        if not s["beats"]: transitions[s["address"]].append("snoop-no-data")
        owner = key[0], s["address"]
        if resp & 3:
            assert owner in copies
            copies[owner] = (1 if resp & 3 == 1 else 2, latest[s["address"]])
        else:
            copies.pop(owner, None)

    by_cycle = {}
    for a in actions:
        by_cycle.setdefault(a["cycle"], [[], []])[1].append(a)
    for r in events:
        if r["handshake"]:
            by_cycle.setdefault(r["cycle"], [[], []])[0].append(r)
    intents = defaultdict(deque)
    for a in actions:
        if a["kind"] != 11: intents[a["node"], a["txn"]].append(a)
    for cycle, (rows, local) in sorted(by_cycle.items()):
        for r in rows:
            d, op = r["direction"], r["opcode"]
            if d in ("cchi_rx_req", "cchi_rx_evt"):
                key = r["node"], r["txn_id"]
                assert key not in requests
                a = intents[key].popleft()
                address = a["address"] & ~63
                if a["kind"] in (9, 10):
                    devices.setdefault(address, bytes(64))
                    if a["kind"] == 10:
                        data = bytearray(devices[address])
                        intent = bytes.fromhex(a["data"])
                        for i in range(64):
                            if a["byte_enable"] & (1 << i): data[i] = intent[i]
                        devices[address] = bytes(data)
                else:
                    initialize(address)
                requests[key] = dict(action=a, beats=set(), dbid=None, resp=None, complete=False)
                if a["kind"] == 3: copies.pop((key[0], address))
                if a["kind"] == 2:
                    l2_data.pop(address, None)
                    undefined_storage.add(address) # MakeUnique installs permission without data.
                    transitions[address].append("makeunique-no-data")
            elif d == "cchi_tx_dat":
                key = r["target"], r["txn_id"]
                q = requests[key]
                a, beat = q["action"], r["data_id"]
                address = a["address"] & ~63
                assert beat in (0, 1) and beat not in q["beats"] and r["resp_err"] == 0
                assert r["trace_tag"] == a["trace_tag"]
                if a["kind"] == 9:
                    start, length = a["address"] & 63, 1 << a["size"]
                    actual = bytes.fromhex(r["data"])
                    for i in range(beat * 32, beat * 32 + 32):
                        if start <= i < start + length: assert actual[i % 32] == devices[address][i]
                    required = {0, 1} if a["size"] == 6 else {(a["address"] >> 5) & 1}
                else:
                    assert a["kind"] in (0, 1, 8)
                    retain = a["kind"] == 1 and copies.get((key[0], address), (0,))[0] == 3
                    if not retain: assert bytes.fromhex(r["data"]) == latest[address][beat * 32:(beat + 1) * 32]
                    assert r["resp"] in ({0, 2} if a["kind"] == 8 else {1, 2, 6} if a["kind"] == 0 else {2, 6})
                    assert q["resp"] in (None, r["resp"]) and q["dbid"] in (None, r["dbid"])
                    q["resp"], q["dbid"] = r["resp"], r["dbid"]
                    required = {0, 1}
                    if a["kind"] != 8:
                        ack = key[0], r["dbid"]
                        assert ack not in acks or acks[ack] == key
                        acks[ack] = key
                q["beats"].add(beat)
                if q["beats"] == required:
                    if a["kind"] in (8, 9): retire(key)
                    else:
                        owner = key[0], address
                        copies[owner] = (3 if retain else {1: 1, 2: 2, 6: 3}[r["resp"]], latest[address])
            elif d == "cchi_tx_rsp":
                key = r["target"], r["txn_id"]
                q = requests[key]
                a = q["action"]
                assert r["resp_err"] == 0 and r["trace_tag"] == a["trace_tag"]
                if op in (2, 3):
                    assert q["dbid"] is None and a["kind"] in (4, 10)
                    q["dbid"] = r["dbid"]
                    q["complete"] = op == 3
                elif a["kind"] == 2:
                    assert op == 1 and r["resp"] == 2
                    acks[key[0], r["dbid"]] = key
                    q["complete"] = True
                else:
                    assert op == (4 if a["kind"] in (5, 6, 7) else 1)
                    if a["kind"] == 7: latest[a["address"]] = memory[a["address"]]
                    if a["kind"] == 5: assert all(p != 3 for (n, addr), (p, data) in copies.items() if addr == a["address"])
                    if a["kind"] in (3, 4, 6, 7):
                        l2_data.pop(a["address"], None)
                        transitions[a["address"]].append({3: "evict-discard", 4: "writeback-discard", 6: "cmo-cleaninvalid-discard", 7: "cmo-makeinvalid-discard"}[a["kind"]])
                    elif a["kind"] == 5:
                        transitions[a["address"]].append("cmo-cleanshared-preserve")
                    if a["kind"] in (6, 7): assert all(addr != a["address"] for n, addr in copies)
                    if a["kind"] in (4, 10): assert q["beats"] == ({0, 1} if a["size"] == 6 else {(a["address"] >> 5) & 1})
                    retire(key)
            elif d == "cchi_rx_rsp" and op == 0:
                key = acks.pop((r["node"], r["txn_id"]))
                q = requests[key]
                a = q["action"]
                assert r["trace_tag"] == a["trace_tag"]
                if a["kind"] == 2:
                    assert q["complete"]
                    latest[a["address"]] = bytes.fromhex(a["data"])
                    copies[key[0], a["address"]] = (3, latest[a["address"]])
                else: assert q["beats"] == {0, 1}
                retire(key)
            elif d == "cchi_tx_snp":
                key = r["target"], r["txn_id"]
                assert key not in snoops and roles[key[0]] == 0
                snoops[key] = dict(address=initialize(r["address"]), beats=set(), opcode=op)
            elif d == "cchi_rx_rsp" and op == 1:
                snoop_finish((r["node"], r["txn_id"]), r["resp"])
            elif d == "cchi_rx_dat":
                key = r["node"], r["txn_id"]
                beat = r["data_id"]
                if op == 3:
                    s = snoops[key]
                    assert beat not in s["beats"] and bytes.fromhex(r["data"]) == latest[s["address"]][beat * 32:(beat + 1) * 32]
                    s["beats"].add(beat)
                    if s["beats"] == {0, 1}:
                        if r["resp"] & 3:
                            l2_data[s["address"]] = latest[s["address"]]
                            transitions[s["address"]].append("snoop-data")
                            undefined_storage.discard(s["address"])
                        snoop_finish(key, r["resp"])
                else:
                    owners = [k for k, q in requests.items() if k[0] == key[0] and q["dbid"] == key[1] and q["action"]["kind"] in (4, 10)]
                    assert len(owners) == 1
                    owner = owners[0]; q = requests[owner]; a = q["action"]
                    assert beat not in q["beats"]
                    q["beats"].add(beat)
                    if a["kind"] == 4 and q["beats"] == {0, 1}:
                        l2_data[a["address"]] = latest[a["address"]]
                        transitions[a["address"]].append("refill")
                        undefined_storage.discard(a["address"])
                        copies.pop((owner[0], a["address"]), None)
                        if q["complete"]: retire(owner)
            elif d == "chi_tx_req":
                if op not in (4, 0x1c, 0x1d): initialize(r["address"])
                if op in (7, 0x26):
                    key = r["node"], r["txn_id"]
                    if key not in down_reads:
                        down_reads[key] = dict(address=r["address"], expected=memory[r["address"]], beats=set())
                    else: assert down_reads[key]["address"] == r["address"]
                if op in (0x17, 0x1b, 0x42, 0x1c, 0x1d):
                    writes[r["node"], r["txn_id"]] = dict(address=r["address"], size=6 if op not in (0x1c, 0x1d) else
                        next(q["action"]["size"] for q in requests.values() if q["action"]["kind"] == 10 and q["action"]["address"] == r["address"]),
                        device=op in (0x1c, 0x1d), beats=set())
            elif d == "chi_rx_dat" and (r["target"], r["txn_id"]) in down_reads:
                key = r["target"], r["txn_id"]
                q = down_reads[key]
                assert op in (4, 11) and r["data_id"] in (0, 2) and r["resp_err"] == 0
                beat = r["data_id"] // 2
                assert beat not in q["beats"]
                assert bytes.fromhex(r["data"]) == q["expected"][beat * 32:(beat + 1) * 32]
                q["beats"].add(beat)
                if q["beats"] == {0, 1}:
                    l2_data[q["address"]] = q["expected"]
                    transitions[q["address"]].append("refill")
                    undefined_storage.discard(q["address"])
                    del down_reads[key]
            elif d == "chi_rx_rsp" and op == 4:
                writes.pop((r["target"], r["txn_id"]), None) # Clean WriteEvictOrEvict Comp branch.
            elif d == "chi_rx_rsp" and op == 5:
                key = r["target"], r["txn_id"]
                assert key in writes
                write_dbids[r["dbid"]] = writes.pop(key)
            elif d == "chi_tx_dat" and op in (2, 3):
                q = write_dbids[r["txn_id"]]
                beat = r["data_id"] // 2
                assert beat not in q["beats"] and r["data_id"] in (0, 2)
                if q["device"]:
                    assert op == 3
                    expected = devices[q["address"] & ~63]
                    actual = bytes.fromhex(r["data"])
                    for i in range(32):
                        if r["byte_enable"] & (1 << i): assert actual[i] == expected[beat * 32 + i]
                else:
                    assert op == 2 and bytes.fromhex(r["data"]) == latest[q["address"]][beat * 32:(beat + 1) * 32]
                q["beats"].add(beat)
                required = {0, 1} if q["size"] == 6 else {(q["address"] >> 5) & 1}
                if q["beats"] == required:
                    if not q["device"]: memory[q["address"]] = latest[q["address"]]
                    del write_dbids[r["txn_id"]]
            elif d == "chi_tx_dat" and op == 1 and r["target"] == 8:
                # Existing lower-Snoop Home sink commits through MemoryAgent;
                # terminal resource drain waits for that actual completion.
                expected = [s["address"] for s in events if s["direction"] == "chi_rx_snp" and s["handshake"] and s["txn_id"] == r["txn_id"]]
                assert len(expected) == 1
                address = expected[0]
                assert bytes.fromhex(r["data"]) == latest[address][r["data_id"] * 16:r["data_id"] * 16 + 32]
                memory[address] = latest[address]
        # Local API stores happen after this cycle's captured wire handshakes.
        for a in local:
            if a["kind"] != 11: continue
            owner = a["node"], a["address"]
            assert copies[owner][0] in (2, 3)
            latest[a["address"]] = bytes.fromhex(a["data"])
            copies[owner] = (3, latest[a["address"]])
    assert not requests and not snoops and not acks and not writes and not write_dbids and not down_reads
    assert all(not queued for queued in intents.values())
    expected = dict(requesters=[dict(node=n, address=a, permission=p, data=data.hex()) for (n, a), (p, data) in sorted(copies.items())],
                    memory=[dict(address=a, data=data.hex()) for a, data in sorted(memory.items())],
                    devices=[dict(address=a, data=data.hex()) for a, data in sorted(devices.items())])
    assert terminal["expected"] == expected, "online architectural model differs from independent replay"
    assert terminal["observed"] == expected, "actual Requester/Home terminal storage differs from independent replay"
    assert terminal["cycle"] >= max(by_cycle)
    return dict(l2UndefinedStorage=sorted(undefined_storage),
                l2Transitions=[dict(address=a, transitions=transitions[a]) for a in sorted(transitions)],
                l2DataVersions=[dict(address=a, data=data.hex()) for a, data in sorted(l2_data.items())],
                requesterFootprint=[dict(node=n, address=a, clientState=1 if p == 1 else 2) for (n, a), (p, data) in sorted(copies.items())],
                requesterCopies=len(copies), memoryLines=len(memory), deviceLines=len(devices),
                scope="terminal Requester identity/permission/data and committed Home/device bytes; internal L2 residency remains open")


def mutations(root, events, seed, topo):
    original = json.loads((root / "architectural-state.json").read_text())
    rejected = 0
    for side in ("expected", "observed"):
        for category in ("requesters", "memory", "devices"):
            variants = []
            entries = original[side][category]
            if entries:
                lost = copy.deepcopy(original); lost[side][category].pop(); variants.append(lost)
                for field in ("data", "address", *(("node", "permission") if category == "requesters" else ())):
                    changed = copy.deepcopy(original)
                    entry = changed[side][category][0]
                    entry[field] = ("ff" if entry[field][:2] != "ff" else "00") + entry[field][2:] if field == "data" else entry[field] + 1
                    variants.append(changed)
            extra = copy.deepcopy(original)
            extra[side][category].append(dict(address=0, data="00" * 64, **(dict(node=0, permission=3) if category == "requesters" else {})))
            variants.append(extra)
            for changed in variants:
                try: check(root, events, seed, topo, changed)
                except (AssertionError, KeyError): rejected += 1
                else: raise AssertionError("architectural checker accepted terminal state mutation")
    return rejected
