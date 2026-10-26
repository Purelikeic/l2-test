"""Bounded accepted transport audit; no Directory residency or data-version oracle.

Credits are fungible by requester/issuer/type. Completion is joined by role,
route and DBID; a numeric TxnID alone cannot merge acquisition and victim work.
"""
import copy
from collections import Counter, defaultdict, deque

READ = {4, 7, 38}
WRITE = {13, 23, 27, 28, 29, 66}
CONTROL = {12, 15, 16, 17}
DEVICE = {4, 28, 29}
FINGERPRINT = ("node", "target", "txn_id", "opcode", "address", "trace_tag", "size",
               "exp_comp_ack", "mem_attr", "order", "ns", "likely_shared")


def role(opcode):
    assert opcode in READ | WRITE | CONTROL, "unsupported downstream operation"
    return "read" if opcode in READ else "write" if opcode in WRITE else "control"


def check(events):
    held, stalls = {}, Counter()
    assert [r["cycle"] for r in events] == sorted(r["cycle"] for r in events)
    assert len({(r["cycle"], r["direction"]) for r in events}) == len(events), "duplicate channel observation"
    for r in events:
        assert r["handshake"] == (r["valid"] and r["ready"])
        fields = {k: v for k, v in r.items() if k not in ("cycle", "sim_time", "ready", "handshake")}
        if r["direction"] in held:
            previous, payload = held[r["direction"]]
            assert r["cycle"] == previous + 1 and fields == payload, "stalled offer changed or disappeared"
        if r["valid"] and not r["ready"]:
            held[r["direction"]] = r["cycle"], fields
            stalls[r["direction"]] += 1
        else:
            held.pop(r["direction"], None)
    assert not held, "terminal stalled offer"
    active, up, up_roles, up_dbids, data_dbids, ack_dbids = {}, {}, {}, {}, {}, {}
    credits = defaultdict(deque)
    completed, retried, upstream = [], [], []
    peak_lines = 0

    def finish(key, q, cycle, form):
        q["finished"] = True
        completed.append(dict(node=q["request"]["node"], txn=q["request"]["txn_id"],
                              opcode=q["request"]["opcode"], address=q["request"]["address"],
                              role=key[2], requestCycle=q["request"]["cycle"], completionCycle=cycle,
                              form=form, retried=q["retry"] is not None))
        if active.get(key) is q: del active[key]

    def receipt_done(q):
        # Order!=0 ReadNoSnp also needs its ReadReceipt (CHI B2.6).
        return not (q["request"]["opcode"] == 4 and q["request"]["order"]) or q["receipt"] is not None

    def request_complete(q):
        if q["request"]["opcode"] in READ:
            return q["beats"] == need_beats(q) and (q["mode"] == "combined" or q["completion"] is not None) and receipt_done(q)
        return q["completion"] is not None or q["dbid"] is not None

    def need_beats(q):
        req = q["request"]
        return {0, 2} if req["opcode"] not in DEVICE or req["size"] == 6 else {((req["address"] >> 5) & 1) * 2}

    def publish_ack(key, q):
        if not q["ack"]: return
        if key[2] == "read":
            if q["beats"] != need_beats(q) or q["mode"] == "separated" and q["completion"] is None: return
            completion = q["completion"] if q["mode"] == "separated" else q["data"]
            target = completion["node"] if q["mode"] == "separated" else completion["home_node"]
            tag = completion["trace_tag"] if q["mode"] == "separated" else q["request"]["trace_tag"] or q["data_tag"]
        else:
            if q["completion"] is None: return
            completion = q["completion"]; target = completion["node"]; tag = completion["trace_tag"]
        ack = q["request"]["node"], target, completion["dbid"]
        assert ack not in ack_dbids or ack_dbids[ack][2] is q, "aliased live completion DBID"
        ack_dbids[ack] = key, tag, q

    # Inputs captured in a cycle are simultaneous. In particular a split write
    # Comp and its final DAT may fire together, despite channel print order.
    def priority(r):
        if r["direction"] == "cchi_tx_rsp" and r["opcode"] == 1: return 3
        if r["direction"] == "chi_tx_rsp": return 3
        if r["direction"].endswith("dat"): return 2
        if r["direction"].endswith("rsp"): return 1
        return 0

    fired = sorted((r for r in events if r["handshake"]), key=lambda r: (r["cycle"], priority(r)))
    for r in fired:
        direction, op = r["direction"], r["opcode"]
        if direction == "cchi_rx_req":
            up_roles[r["node"], r["txn_id"]] = "other"
        if direction == "chi_tx_req":
            family = role(op)
            for flag in ("allow_retry", "exp_comp_ack", "ns", "likely_shared"):
                assert type(r[flag]) is bool, "missing typed request observation"
            assert 0 <= r["p_crd_type"] < 16 and 0 <= r["size"] <= 6
            assert r["exp_comp_ack"] == (op in (7, 38, 12, 66))
            assert r["node"] == 1 and r["target"] == 8, "request left the bounded Home route"
            if op not in DEVICE:
                assert r["size"] == 6 and r["address"] % 64 == 0 and r["mem_attr"] & 4 and r["order"] == 0 and not r["ns"]
            key = r["node"], r["txn_id"], family
            if op == 4:
                # AnserL2/CoupledL2 MMIOBridge: no ReadNoSnp while an issued ordered read lacks
                # its ReadReceipt; an accepted RetryAck suspends that until the reissue.
                for k, q in active.items():
                    waiting = q["request"]["opcode"] == 4 and q["request"]["order"] and q["receipt"] is None
                    if k != key and waiting and not (q["retry"] is not None and not q["reissued"]):
                        raise AssertionError("ReadNoSnp issued while an ordered read awaits ReadReceipt")
            if key in active and not r["allow_retry"]:
                q = active[key]
                retry = q["retry"]
                assert retry is not None and not q["reissued"] and not r["allow_retry"]
                assert all(r[k] == q["request"][k] for k in FINGERPRINT), "reissue changed its request"
                assert r["p_crd_type"] == retry["p_crd_type"] and r["cycle"] > retry["cycle"]
                pool = credits[r["node"], retry["node"], retry["p_crd_type"]]
                assert pool and pool[0]["cycle"] < r["cycle"], "reissue lacks an accepted issuer/type credit"
                credit = pool.popleft()
                q["reissued"] = True
                retried.append(dict(node=r["node"], txn=r["txn_id"], opcode=op, address=r["address"],
                                    issuer=retry["node"], type=retry["p_crd_type"],
                                    requestCycle=q["request"]["cycle"], retryCycle=retry["cycle"],
                                    creditCycle=credit["cycle"], reissueCycle=r["cycle"],
                                    earlyCredit=credit["cycle"] < retry["cycle"]))
            else:
                if key in active:
                    assert request_complete(active[key]), "request TxnID reused before completion delivery"
                assert r["p_crd_type"] == 0 and r["allow_retry"], "invalid initial retry contract"
                acquisition = active.get((r["node"], r["txn_id"], "read")) if family == "write" else None
                active[key] = dict(request=r, retry=None, reissued=False, completion=None, dbid=None,
                                   mode=None, beats=set(), data=None, data_tag=False, ack=r["exp_comp_ack"], acquisition=acquisition,
                                   receipt=None)
        elif direction == "chi_rx_rsp":
            assert r["resp_err"] == 0 and 0 <= r["p_crd_type"] < 16
            if op == 7:
                assert r["target"] == 1
                credits[r["target"], r["node"], r["p_crd_type"]].append(r)
                continue # PCrdGrant.TxnID is intentionally not a matching key.
            assert op in (3, 4, 5, 6, 8, 11, 14), "unsupported downstream response"
            candidates = []
            for key, q in active.items():
                if key[:2] != (r["target"], r["txn_id"]): continue
                if op == 3 and q["retry"] is None and q["completion"] is None and not q["beats"] and q["dbid"] is None \
                        and q["receipt"] is None:
                    candidates.append((key, q))
                elif op == 8 and key[2] == "read" and q["request"]["opcode"] == 4 and q["request"]["order"] \
                        and q["receipt"] is None: candidates.append((key, q))
                elif op == 11 and key[2] == "read" and q["completion"] is None: candidates.append((key, q))
                elif op in (5, 6, 14) and key[2] == "write" and q["dbid"] is None: candidates.append((key, q))
                elif op == 4 and key[2] in ("control", "write") and q["completion"] is None: candidates.append((key, q))
            assert len(candidates) == 1, "response lacks an unambiguous transaction role"
            key, q = candidates[0]
            assert q["retry"] is None or q["reissued"], "completion preceded authorized reissue"
            if op == 3:
                q["retry"] = r
            elif op == 8:
                q["receipt"] = r
                if q["mode"] == "combined" and q["beats"] == need_beats(q): finish(key, q, r["cycle"], "combined")
                continue
            elif op == 11:
                assert q["mode"] in (None, "separated")
                q["mode"] = "separated"; q["completion"] = r
            elif op in (5, 6, 14):
                assert q["request"]["opcode"] in (23, 27, 28, 29, 66)
                assert op != 14 or q["request"]["order"], "DBIDRespOrd for an unordered write"
                assert op == 5 or q["request"]["opcode"] in (28, 29), "unsupported cacheable DBID-only completion"
                q["dbid"] = r; q["mode"] = "copyback" if op == 5 else "split-write"
                if op == 5:
                    assert q["completion"] is None
                    q["completion"] = r
                q["ack"] = False # WriteEvictOrEvict DBID branch cancels its CompAck.
                dbid = r["target"], r["node"], r["dbid"]
                assert dbid not in data_dbids
                data_dbids[dbid] = key, q
            else:
                assert key[2] == "control" or q["request"]["opcode"] in (13, 28, 29, 66)
                q["completion"] = r
                if q["request"]["opcode"] in (28, 29): q["mode"] = "split-write"
                else: q["mode"] = "comp"
                if (not q["ack"] and key[2] == "control") or q["request"]["opcode"] == 13:
                    finish(key, q, r["cycle"], "comp")
                    continue
            publish_ack(key, q)
            if key[2] == "read" and not q["ack"] and q["completion"] is not None and q["beats"] == need_beats(q):
                finish(key, q, r["cycle"], q["mode"])
            elif q["request"]["opcode"] in (28, 29) and q["completion"] is not None and q["beats"] == need_beats(q):
                finish(key, q, r["cycle"], q["mode"])
        elif direction == "chi_rx_dat":
            key = r["target"], r["txn_id"], "read"
            assert key in active, "DAT lacks accepted read identity"
            q = active[key]
            assert q["retry"] is None or q["reissued"]
            assert op in (4, 11) and r["resp_err"] == 0 and r["home_node"] == q["request"]["target"]
            mode = "combined" if op == 4 else "separated"
            assert q["mode"] in (None, mode), "mixed combined/separated data"
            q["mode"] = mode
            assert r["data_id"] in need_beats(q) and r["data_id"] not in q["beats"]
            if q["data"] is not None:
                assert all(r[k] == q["data"][k] for k in ("dbid", "home_node", "resp")), "DAT changed line identity/state"
            q["data"] = r; q["data_tag"] |= r["trace_tag"]; q["beats"].add(r["data_id"])
            assert len(bytes.fromhex(r["data"])) == 32
            publish_ack(key, q)
            if not q["ack"] and q["beats"] == need_beats(q) and (mode == "combined" or q["completion"] is not None) \
                    and receipt_done(q):
                finish(key, q, r["cycle"], mode)
        elif direction == "chi_tx_rsp" and op == 2:
            ack = r["node"], r["target"], r["txn_id"]
            assert ack in ack_dbids, "CompAck preceded completion or used the wrong route/DBID"
            key, tag, q = ack_dbids.pop(ack)
            assert r["trace_tag"] == tag and r["resp_err"] == 0
            if key[2] == "write":
                assert q["acquisition"] is None or q["acquisition"].get("finished", False), "victim CompAck preceded acquisition CompAck"
            finish(key, q, r["cycle"], q["mode"])
        elif direction == "chi_tx_dat" and op in (2, 3):
            dbid = r["node"], r["target"], r["txn_id"]
            assert dbid in data_dbids, "write data preceded DBID or changed its route"
            key, q = data_dbids[dbid]
            assert op == (3 if q["request"]["opcode"] in DEVICE else 2)
            assert r["trace_tag"] == q["dbid"]["trace_tag"] and r["resp_err"] == 0
            assert r["data_id"] in need_beats(q) and r["data_id"] not in q["beats"]
            if op == 2:
                assert r["byte_enable"] == 0xffffffff and r["resp"] in (1, 2, 5, 6)
                if q["request"]["opcode"] != 66: assert r["resp"] & 4
            q["beats"].add(r["data_id"])
            if q["beats"] == need_beats(q):
                del data_dbids[dbid]
                if q["completion"] is not None: finish(key, q, r["cycle"], q["mode"])
        elif direction == "cchi_rx_evt":
            assert op in (0, 1), "unsupported Compact EVT"
            key = r["node"], r["txn_id"]
            up_roles[key] = "writeback" if op == 1 else "other"
            if op != 1: continue
            assert key not in up
            up[key] = dict(request=r, grant=None, complete=False, beats=set())
        elif direction == "cchi_tx_rsp" and up_roles.get((r["target"], r["txn_id"])) == "writeback":
            key = r["target"], r["txn_id"]
            assert key in up, "duplicate upstream WriteBack completion"
            q = up[key]
            assert r["node"] == q["request"]["target"] and r["trace_tag"] == q["request"]["trace_tag"] and r["resp_err"] == 0
            if op in (2, 3):
                assert q["grant"] is None
                q["grant"] = r; q["complete"] = op == 3
                dbid = r["target"], r["node"], r["dbid"]
                assert dbid not in up_dbids; up_dbids[dbid] = key
            else:
                assert op == 1 and not q["complete"] and q["beats"] == {0, 1}
                q["complete"] = True
                upstream.append(dict(node=key[0], txn=key[1], combined=False, completionCycle=r["cycle"]))
                del up[key]
        elif direction == "cchi_rx_dat" and op == 2:
            dbid = r["node"], r["target"], r["txn_id"]
            assert dbid in up_dbids, "upstream WriteBack data lacks an accepted DBID"
            key = up_dbids[dbid]; q = up[key]
            assert r["data_id"] in (0, 1) and r["data_id"] not in q["beats"]
            assert r["trace_tag"] == q["request"]["trace_tag"] and r["resp_err"] == 0 and r["resp"] == 6 and r["byte_enable"] == 0xffffffff
            q["beats"].add(r["data_id"])
            if q["beats"] == {0, 1}:
                del up_dbids[dbid]
                if q["complete"]:
                    upstream.append(dict(node=key[0], txn=key[1], combined=True, completionCycle=r["cycle"]))
                    del up[key]
        peak_lines = max(peak_lines, len({q["request"]["address"] for q in active.values()}))
    assert not active and not up and not up_dbids and not data_dbids and not ack_dbids, "terminal transport obligations remain"
    assert not any(credits.values()), "terminal issuer/type credits remain"
    return dict(transactions=len(completed), completions=completed, retries=len(retried), retryEvidence=retried,
                earlyCreditReissues=sum(r["earlyCredit"] for r in retried), peakLiveLines=peak_lines,
                completionForms=dict(Counter(q["form"] for q in completed)), stalls=dict(stalls),
                upstreamWritebacks=upstream,
                scope="accepted normal-profile Retry/credit/completion/stall/line interleave; no residency or full error/overlap equivalence")


def mutations(events):
    variants, covered = [], set()
    for index, row in enumerate(events):
        if not row["handshake"]: continue
        token = row["direction"], row["opcode"], row.get("allow_retry") if row["direction"] == "chi_tx_req" else None
        if token in covered: continue
        covered.add(token)
        if token[0] == "chi_tx_req":
            for field, value in (("p_crd_type", 16), ("exp_comp_ack", not row["exp_comp_ack"]), ("opcode", 0x30)):
                changed = copy.deepcopy(events); changed[index][field] = value; variants.append(changed)
        elif token[:2] in (("chi_rx_rsp", 3), ("chi_rx_rsp", 7)):
            changed = copy.deepcopy(events); changed[index]["p_crd_type"] = 16; variants.append(changed)
        elif token[:2] in (("chi_rx_dat", 4), ("chi_rx_dat", 11), ("cchi_rx_dat", 2)):
            changed = copy.deepcopy(events); changed[index]["data_id"] = 7; variants.append(changed)
        elif token[:2] == ("chi_tx_rsp", 2):
            for field in ("txn_id", "target", "trace_tag"):
                changed = copy.deepcopy(events)
                changed[index][field] = not row[field] if field == "trace_tag" else row[field] + 2048
                variants.append(changed)
        elif token[:2] in (("chi_tx_dat", 2), ("chi_tx_dat", 3)):
            changed = copy.deepcopy(events); changed[index]["txn_id"] += 4096; variants.append(changed)
    for index, row in enumerate(events):
        if row["valid"] and not row["ready"]:
            changed = copy.deepcopy(events); changed[index]["txn_id"] += 4096
            variants.append(changed)
            break
    for changed in variants:
        try: check(changed)
        except (AssertionError, KeyError, IndexError, TypeError, ValueError): pass
        else: raise AssertionError("transport checker accepted a mutated credit/completion/stall")
    return len(variants)
