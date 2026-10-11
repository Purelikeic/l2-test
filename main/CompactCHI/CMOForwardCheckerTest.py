"""Host-only CMO/forward interaction cases; no claim of native joint stimulus."""
import copy
import json
from pathlib import Path
import runpy
import sys

HERE = Path(__file__).resolve().parent
cmo_check = runpy.run_path(str(HERE / "check-cmo.py"))["check"]
forward_check = runpy.run_path(str(HERE / "check-forward.py"))["check"]
interaction_check = runpy.run_path(str(HERE / "check-cmo-forward.py"))["check"]
root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
(root / "topology.json").write_text(json.dumps(dict(nodes=[dict(id=0, kind=0)])))


def event(cycle, direction, node, target, txn, opcode, **fields):
    return dict(cycle=cycle, direction=direction, node=node, target=target,
                txn_id=txn, opcode=opcode, valid=True, handshake=True, trace_tag=False, resp_err=0, **fields)


def fixture(kind, timing, downstream_txn, same_line=False):
    address = 4160 if same_line else 4096
    req, home, comp = {"before": (10, 14, 16), "overlap": (19, 21, 22), "after": (24, 28, 30)}[timing]
    actions = [dict(kind=kind, node=0, txn=53, address=address, cycle=req-1, trace_tag=False)]
    up, down = {5: (12, 16), 6: (13, 17), 7: (14, 15)}[kind]
    rows = [event(req, "cchi_rx_req", 0, 1, 53, up, address=address),
            event(req+1, "chi_tx_req", 1, 8, downstream_txn, down, address=address),
            event(home, "chi_rx_rsp", 8, 1, downstream_txn, 4),
            event(comp, "cchi_tx_rsp", 1, 0, 53, 4),
            event(18, "chi_rx_snp", 8, 1, 53, 23, address=4160, forward_node=12, forward_txn=501),
            event(20, "chi_tx_dat", 1, 12, 501, 4, dbid=53, data_id=0),
            event(21, "chi_tx_rsp", 1, 8, 53, 9),
            event(22, "chi_tx_dat", 1, 12, 501, 4, dbid=53, data_id=2)]
    for row in rows:
        if row["direction"] in ("chi_rx_snp", "chi_tx_dat", "chi_tx_rsp"):
            row["trace_tag"] = True
    return sorted(rows, key=lambda r: r["cycle"]), actions


def audit(rows, actions):
    (root / "actions.jsonl").write_text("".join(json.dumps(a) + "\n" for a in actions))
    cmo = cmo_check(root, rows, "host-cmo-forward")
    forward = forward_check(root, rows, "lower-snoop")
    return interaction_check(root, rows, "host-cmo-forward", cmo, forward)


passed = rejected = 0
for kind in (5, 6, 7):
    # Equal numbers are legal across the upstream CMO, downstream CMO,
    # forward TxnID and original Home Snoop DBID namespaces.
    for downstream_txn in (53, 501):
        for timing, same_line in (("before", False), ("overlap", False), ("after", False),
                                  ("before", True), ("after", True)):
            rows, actions = fixture(kind, timing, downstream_txn, same_line)
            result = audit(rows, actions)
            assert result["interactions"] == 1
            pair = result["evidence"][0]
            assert pair["sameLine"] == same_line
            assert pair["relation"] == {"before": "cmo-before-forward", "overlap": "independent-line-overlap",
                                        "after": "cmo-after-forward"}[timing]
            if timing == "overlap":
                assert pair["compCMOCycle"] < pair["forwardAckCycle"]
            passed += 1
        rows, actions = fixture(kind, "overlap", downstream_txn)
        variants = []
        # Neither the external CompAck nor SnpRespFwd can substitute for the
        # accepted CMO's Home Comp or upstream Compact CompCMO.
        for direction, source, target, opcode in (("chi_rx_rsp", 12, 8, 2),
                                                   ("chi_tx_rsp", 1, 8, 9),
                                                   ("chi_rx_rsp", 8, 1, 2)):
            changed = copy.deepcopy(rows)
            response = next(r for r in changed if r["direction"] == "chi_rx_rsp")
            response.update(direction=direction, node=source, target=target, opcode=opcode)
            variants.append((changed, actions))
        for field, value in (("node", 12), ("target", 8), ("txn_id", 501), ("opcode", 2), ("trace_tag", True)):
            changed = copy.deepcopy(rows)
            response = next(r for r in changed if r["direction"] == "cchi_tx_rsp")
            response[field] = value
            variants.append((changed, actions))
        # Observe the external acknowledgment on every canonical DUT RSP
        # direction: it must stay outside these wires even with valid CMO.
        for direction in ("chi_rx_rsp", "chi_tx_rsp", "cchi_rx_rsp", "cchi_tx_rsp"):
            for handshake in (True, False):
                changed = copy.deepcopy(rows)
                external = event(23, direction, 12, 8, 53, 2)
                external["handshake"] = handshake
                changed.append(external)
                variants.append((sorted(changed, key=lambda r: r["cycle"]), actions))
        for field, value in (("address", 4096), ("forward_node", 8), ("forward_txn", 53),
                             ("txn_id", 501), ("trace_tag", False)):
            changed = copy.deepcopy(rows)
            snoop = next(r for r in changed if r["direction"] == "chi_rx_snp")
            snoop[field] = value
            variants.append((changed, actions))
        variants.append(fixture(kind, "overlap", downstream_txn, same_line=True))
        # Same-line CMO may start only after the external obligation retires
        # within this serialized model, including the acknowledgment cycle.
        changed, intents = fixture(kind, "after", downstream_txn, same_line=True)
        next(r for r in changed if r["direction"] == "cchi_rx_req")["cycle"] = 23
        intents[0]["cycle"] = 22
        variants.append((sorted(changed, key=lambda r: r["cycle"]), intents))
        for changed, intents in variants:
            try:
                audit(changed, intents)
            except (AssertionError, KeyError):
                rejected += 1
            else:
                raise AssertionError("CMO/forward audit accepted an invalid interaction")

result = dict(state="passed", cases=passed, mutationRejections=rejected,
              scope="host-only CMO/forward; fixed nodes 12/8; no native CMO/DCT overlap claim",
              exclusions=["nested same-line CMO/DCT", "external forward residency", "multiple forward obligations"])
(root / "cmo-forward-host.json").write_text(json.dumps(result, indent=2) + "\n")
print("CMO/forward host interaction:", passed, "cases and", rejected, "mutations passed")
