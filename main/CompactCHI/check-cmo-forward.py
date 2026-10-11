"""CMO interaction audit using the existing, separately validated forward evidence.

Only the fixed 12 -> 8 external endpoint is modeled. Independent-line overlap
and serialized same-line traffic are checked; nested same-line CMO/DCT needs an
independent external residency model and remains outside this bounded fixture.
"""
import json


def check(root, events, profile, cmo, forward):
    fired = [r for r in events if r["handshake"]]
    # This host-to-host acknowledgment is never a DUT flit, including a stalled
    # offer. Its numbers may legally alias either CMO transaction namespace.
    assert not any(r.get("valid", r["handshake"]) and r["node"] == 12 and
                   r["target"] == 8 and r["opcode"] == 2 for r in events), \
        "external forward CompAck appeared on a DUT channel"
    pairs = []
    if forward["observations"]:
        ack = [json.loads(line) for line in (root / "forward.jsonl").read_text().splitlines()][-1]
        snoops = [r for r in fired if r["direction"] == "chi_rx_snp" and
                  r["node"] == 8 and r["target"] == 1 and r["opcode"] == 23 and
                  r["txn_id"] == forward["dbid"] and r["forward_node"] == 12 and
                  r["forward_txn"] == forward["forwardTxn"]]
        assert len(snoops) == 1, "forward evidence lacks its accepted fixed-endpoint Snoop"
        snoop = snoops[0]
        assert snoop["trace_tag"] == ack["trace_tag"] and snoop["cycle"] < ack["cycle"]
        # Do not derive any expected bytes, residency or CMO result from DCT.
        for cm in cmo["evidence"]:
            same_line = cm["address"] == snoop["address"]
            overlap = cm["requestCycle"] <= ack["cycle"] and snoop["cycle"] <= cm["compCMOCycle"]
            assert not (same_line and overlap), \
                "nested same-line CMO/forward traffic exceeds the bounded interaction model"
            relation = ("independent-line-overlap" if overlap else
                        "cmo-before-forward" if cm["compCMOCycle"] < snoop["cycle"] else
                        "cmo-after-forward")
            pairs.append(dict(kind=cm["kind"], node=cm["node"], txn=cm["txn"], address=cm["address"],
                              downstreamTxn=cm["downstreamTxn"], compCMOCycle=cm["compCMOCycle"],
                              forwardTxn=forward["forwardTxn"], forwardDBID=forward["dbid"],
                              forwardAddress=snoop["address"], forwardAckCycle=ack["cycle"],
                              sameLine=same_line, relation=relation))
    return dict(cmoActions=cmo["cmoActions"], forwardObligations=forward["delayedAcks"],
                interactions=len(pairs), evidence=pairs,
                scope="fixed nodes 12/8; independent-line overlap and serialized same-line CMO/forward; external CompAck stays outside DUT")
