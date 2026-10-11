"""Independent bounded CMO action/nested-Snoop ordering checker."""
import copy
import json
from pathlib import Path

CMO = {5: 0x10, 6: 0x11, 7: 0x0f}

def check(root, events, profile):
    actions = [json.loads(line) for line in (root / "actions.jsonl").read_text().splitlines()]
    cm_actions = [a for a in actions if a["kind"] in CMO]
    if not cm_actions:
        return dict(cmoActions=0, nestedSnoops=0, mutationRejections=0, scope="no CMO actions in this profile")
    fired = [r for r in events if r["handshake"]]
    requests = [r for r in fired if r["direction"] == "cchi_rx_req"]
    down = [r for r in fired if r["direction"] == "chi_tx_req"]
    responses = [r for r in fired if r["direction"] == "chi_rx_rsp" and r["opcode"] == 4]
    used = set(); evidence=[]
    for action in cm_actions:
        req = next((r for r in requests if r["node"] == action["node"] and r["txn_id"] == action["txn"] and r["address"] == action["address"]), None)
        assert req is not None and req["cycle"] == action["cycle"] + 1 and req["opcode"] == {5: 12, 6: 13, 7: 14}[action["kind"]]
        candidate = next((r for r in down if r["address"] == action["address"] and r["opcode"] == CMO[action["kind"]] and r["cycle"] > req["cycle"] and (r["cycle"], r["txn_id"]) not in used), None)
        assert candidate is not None
        used.add((candidate["cycle"], candidate["txn_id"]))
        rsp = next((r for r in responses if r["txn_id"] == candidate["txn_id"] and r["cycle"] > candidate["cycle"]), None)
        assert rsp is not None and rsp["cycle"] > candidate["cycle"]
        nested = [r for r in fired if r["direction"] == "cchi_tx_snp" and r["address"] == action["address"] and candidate["cycle"] <= r["cycle"] < rsp["cycle"]]
        for snoop in nested:
            completions = [r for r in fired if r["cycle"] > snoop["cycle"] and r["cycle"] < rsp["cycle"] and r["direction"] in ("cchi_rx_rsp", "cchi_rx_dat") and r["txn_id"] == snoop["txn_id"]]
            assert completions, "nested Snoop remained live when CompCMO was returned"
        evidence.append(dict(kind=action["kind"], node=action["node"], txn=action["txn"], requestCycle=req["cycle"],
                             downstreamOpcode=candidate["opcode"], downstreamTxn=candidate["txn_id"],
                             responseCycle=rsp["cycle"], nestedSnoops=len(nested)))
    return dict(cmoActions=len(evidence), nestedSnoops=sum(e["nestedSnoops"] for e in evidence), evidence=evidence,
                scope="accepted CMO to downstream CompCMO and bounded nested-Snoop ordering")

def mutations(root, events, profile):
    actions_path = root / "actions.jsonl"
    original = [json.loads(line) for line in actions_path.read_text().splitlines()]
    if not any(a["kind"] in CMO for a in original): return 0
    rejected=0
    index=next(i for i,a in enumerate(original) if a["kind"] in CMO)
    for field, value in (("kind", 5 if original[index]["kind"] != 5 else 6),
                         ("address", original[index]["address"] + 64),
                         ("txn", original[index]["txn"] + 1),
                         ("cycle", original[index]["cycle"] + 1)):
        changed=copy.deepcopy(original); changed[index][field]=value
        backup=actions_path.read_text(); actions_path.write_text("".join(json.dumps(a)+"\n" for a in changed))
        try: check(root,events,profile)
        except (AssertionError,KeyError): rejected+=1
        else: raise AssertionError("CMO checker accepted a mutated action/order")
        finally: actions_path.write_text(backup)
    return rejected
