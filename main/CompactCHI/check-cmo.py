"""Supplemental CMO trace audit; online C++ ArchitecturalBoard owns completion checks."""
import copy
import json

CMO = {5: (12, 0x10), 6: (13, 0x11), 7: (14, 0x0f)}


def check(root, events, profile, actions=None):
    if actions is None:
        actions = [json.loads(line) for line in (root / "actions.jsonl").read_text().splitlines()]
    cm_actions = [a for a in actions if a["kind"] in CMO]
    fired = [r for r in events if r["handshake"]]
    evidence = []
    used = set()
    for action in cm_actions:
        upstream_op, downstream_op = CMO[action["kind"]]
        # API enqueue can be followed by arbitrary REQ backpressure. Select an
        # unused handshake, then bound every reused wire identity by CompCMO.
        req_index = next((i for i, r in enumerate(fired) if i not in used and
                          r["direction"] == "cchi_rx_req" and r["node"] == action["node"] and
                          r["txn_id"] == action["txn"] and r["address"] == action["address"] and
                          r["cycle"] > action["cycle"]), None)
        assert req_index is not None, "CMO intent lacks its accepted REQ"
        used.add(req_index)
        req = fired[req_index]
        assert req["opcode"] == upstream_op and req["trace_tag"] == action["trace_tag"]
        rsp_index = next((i for i in range(req_index + 1, len(fired)) if
                          fired[i]["direction"] == "cchi_tx_rsp" and
                          fired[i]["target"] == action["node"] and fired[i]["txn_id"] == action["txn"]), None)
        assert rsp_index is not None, "CMO lacks upstream Compact CompCMO"
        rsp = fired[rsp_index]
        assert rsp["node"] == req["target"] and rsp["opcode"] == 4 and rsp["resp_err"] == 0
        assert rsp["trace_tag"] == action["trace_tag"] and rsp["cycle"] > req["cycle"]
        window = fired[req_index + 1:rsp_index]
        pending = {}
        nested = []
        downstream = {}
        home_done = []
        for r in window:
            d = r["direction"]
            if d == "chi_tx_req" and r["address"] == action["address"] and r["opcode"] == downstream_op:
                key = r["node"], r["txn_id"]
                if key in downstream: assert downstream[key]["target"] == r["target"]
                downstream[key] = r # Retry reissues keep the same CMO identity.
            elif d == "chi_rx_rsp" and r["opcode"] == 4 and (r["target"], r["txn_id"]) in downstream:
                original = downstream.pop((r["target"], r["txn_id"]))
                assert r["node"] == original["target"] and r["resp_err"] == 0
                assert r["cycle"] > original["cycle"]
                home_done.append((original, r))
            elif d == "cchi_tx_snp" and r["address"] == action["address"]:
                key = r["target"], r["txn_id"]
                assert key not in pending and r["node"] == req["target"] and r["opcode"] in (0, 1, 2, 3)
                pending[key] = dict(request=r, beats=set(), resp=None)
                nested.append(r)
            elif d in ("cchi_rx_rsp", "cchi_rx_dat") and (r["node"], r["txn_id"]) in pending:
                key = r["node"], r["txn_id"]
                snoop = pending[key]
                assert r["target"] == snoop["request"]["node"] and r["resp_err"] == 0
                assert r["trace_tag"] == snoop["request"]["trace_tag"]
                if d == "cchi_rx_rsp":
                    assert r["opcode"] == 1 and not snoop["beats"], "Snoop mixed RSP and partial DAT"
                    del pending[key]
                else:
                    assert r["opcode"] == 3 and r["data_id"] in (0, 1)
                    assert r["data_id"] not in snoop["beats"] and snoop["resp"] in (None, r["resp"])
                    snoop["resp"] = r["resp"]
                    snoop["beats"].add(r["data_id"])
                    if snoop["beats"] == {0, 1}: del pending[key]
        assert not pending, "CompCMO preceded complete nested Snoop RSP or both DAT beats"
        assert len(home_done) == 1 and not downstream, "CompCMO lacks its completed downstream Home CMO"
        down_req, home_rsp = home_done[0]
        assert rsp["cycle"] >= home_rsp["cycle"]
        evidence.append(dict(kind=action["kind"], node=action["node"], txn=action["txn"],
                             requestCycle=req["cycle"], downstreamOpcode=down_req["opcode"],
                             downstreamTxn=down_req["txn_id"], homeCompCycle=home_rsp["cycle"],
                             compCMOCycle=rsp["cycle"], nestedSnoops=len(nested)))
    return dict(cmoActions=len(evidence), nestedSnoops=sum(e["nestedSnoops"] for e in evidence),
                evidence=evidence, scope="upstream Compact CompCMO after Home Comp and full nested Snoop retirement")


def mutations(root, events, profile):
    actions = [json.loads(line) for line in (root / "actions.jsonl").read_text().splitlines()]
    if not any(a["kind"] in CMO for a in actions): return 0
    variants = []
    index = next(i for i, a in enumerate(actions) if a["kind"] in CMO)
    for field, value in (("kind", 5 if actions[index]["kind"] != 5 else 6),
                         ("address", actions[index]["address"] + 64),
                         ("node", actions[index]["node"] + 1),
                         ("cycle", max(r["cycle"] for r in events) + 1)):
        changed = copy.deepcopy(actions); changed[index][field] = value
        variants.append((events, changed))
    first = check(root, events, profile, actions)["evidence"][0]
    changed = [copy.deepcopy(r) for r in events if not (r["handshake"] and r["direction"] == "cchi_tx_rsp" and
               r["target"] == first["node"] and r["txn_id"] == first["txn"] and r["cycle"] == first["compCMOCycle"])]
    variants.append((changed, actions))
    for field, value in (("target", first["node"] + 1), ("opcode", 1), ("trace_tag", not actions[index]["trace_tag"]),
                         ("cycle", first["requestCycle"])):
        changed = copy.deepcopy(events)
        row = next(r for r in changed if r["handshake"] and r["direction"] == "cchi_tx_rsp" and
                   r["target"] == first["node"] and r["txn_id"] == first["txn"] and r["cycle"] == first["compCMOCycle"])
        row[field] = value; variants.append((changed, actions))
    # Remove one beat from a data-returning nested Snoop; a single beat must
    # never satisfy the obligation even though the final requester is clean.
    nested_data = [r for r in events if r["handshake"] and r["direction"] == "cchi_rx_dat" and r["opcode"] == 3 and
                   first["requestCycle"] < r["cycle"] < first["compCMOCycle"]]
    if nested_data:
        victim = nested_data[-1]
        variants.append(([r for r in copy.deepcopy(events) if r != victim], actions))
    for changed, intents in variants:
        try: check(root, changed, profile, intents)
        except (AssertionError, KeyError): pass
        else: raise AssertionError("CMO audit accepted a mutated intent/completion/Snoop obligation")
    return len(variants)
