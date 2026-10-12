"""Fixed-node multi-forward obligations, including Home/DCT join and reuse.

Inputs are independently specified stable-line Snoop intents and versions. This
is a bounded external endpoint audit, not arbitrary network topology modeling.
"""
import copy


def check(events, observations, intents, max_active=2):
    assert max_active == 2, "this fixture proves only two concurrent forward obligations"
    fired = [r for r in events if r["handshake"]]
    assert len({(r["cycle"],r["direction"]) for r in fired}) == len(fired), "multiple flits on one scalar DUT channel"
    assert not any(r["node"] == 12 and r["target"] == 8 and r["opcode"] == 2 for r in fired), "external CompAck was injected into DUT"
    assert [r["cycle"] for r in observations] == sorted(r["cycle"] for r in observations)
    requests = iter(intents)
    active, dbids, completed = {}, {}, []
    by_cycle, logged_data, logged_ack = {}, {}, {}
    for r in fired: by_cycle.setdefault(r["cycle"], []).append(r)
    for r in observations:
        if r["kind"] == "data":
            key = r["cycle"], r["txn"], r["beat"]
            assert key not in logged_data; logged_data[key] = r
        else:
            assert r["kind"] == "ack" and r["cycle"] not in logged_ack
            logged_ack[r["cycle"]] = r
    peak = 0
    end = max([r["cycle"] for r in fired] + [r["cycle"] for r in observations], default=0)

    def retire(key):
        q = active[key]
        if q["home"] and q["ack"]:
            completed.append(dict(address=q["intent"]["address"], forwardTxn=key,
                                  dbid=q["intent"]["txn_id"], ackCycle=q["ackCycle"],
                                  lastDataCycle=q["last"], traceTag=q["intent"]["trace_tag"]))
            del dbids[q["intent"]["txn_id"]]; del active[key]

    for cycle in range(end+1):
        # The fixed host endpoint services one eligible acknowledgment per tick.
        # Two lines ready together require distinct following service cycles.
        ready = sorted(k for k,q in active.items() if q["beats"] == {0,2} and not q["ack"] and q["last"] < cycle)
        ack = logged_ack.pop(cycle, None)
        if ready:
            key = ready[0]; q = active[key]; intent = q["intent"]
            assert ack is not None, "eligible endpoint acknowledgment missing"
            assert (ack["source"],ack["target"],ack["txn"],ack["dbid"],ack["beat"],ack["trace_tag"]) == (12,8,intent["txn_id"],intent["txn_id"],3,intent["trace_tag"])
            q["ack"] = True; q["ackCycle"] = cycle; retire(key)
        else: assert ack is None, "premature or extra forward acknowledgment"
        for r in by_cycle.get(cycle, []):
            d = r["direction"]
            if d == "chi_rx_snp":
                intent = next(requests)
                assert all(r[k] == intent[k] for k in ("node","target","txn_id","opcode","address","trace_tag","forward_node","forward_txn"))
                assert r["node"] == 8 and r["target"] == 1 and r["forward_node"] == 12 and r["opcode"] == 23
                key = intent["forward_txn"]
                assert key not in active and intent["txn_id"] not in dbids, "reused live forward/Home identity"
                assert not any(q["intent"]["address"] == intent["address"] for q in active.values()), "same-line forward obligations overlap"
                assert intent["before"] in (1,2,3) and len(bytes.fromhex(intent["data"])) == 64
                active[key] = dict(intent=intent,beats=set(),home=False,ack=False,last=None)
                dbids[intent["txn_id"]] = key
                peak = max(peak,len(active)); assert peak <= max_active
            elif d == "chi_tx_dat":
                assert r["node"] == 1 and r["target"] == 12 and r["opcode"] == 4 and r["txn_id"] in active
                key = r["txn_id"]; q = active[key]; intent = q["intent"]
                assert not q["ack"] and r["data_id"] in (0,2) and r["data_id"] not in q["beats"]
                expected_resp = 6 if intent["before"] == 3 else 2
                assert r["dbid"] == intent["txn_id"] and r["home_node"] == 8 and r["trace_tag"] == intent["trace_tag"]
                assert r["resp"] == expected_resp and r["resp_err"] == 0 and r["byte_enable"] == 0xffffffff
                beat = r["data_id"] // 2; expected = bytes.fromhex(intent["data"])[beat*32:(beat+1)*32]
                assert bytes.fromhex(r["data"]) == expected
                row = logged_data.pop((cycle,key,beat),None)
                assert row is not None and row["kind"] == "data"
                assert (row["txn"],row["dbid"],row["source"],row["target"],row["trace_tag"]) == (key,intent["txn_id"],1,12,intent["trace_tag"])
                q["beats"].add(r["data_id"]); q["last"] = cycle
            elif d == "chi_tx_rsp":
                assert r["node"] == 1 and r["target"] == 8 and r["txn_id"] in dbids
                key = dbids[r["txn_id"]]; q = active[key]; intent=q["intent"]
                expected_resp = 0
                fwd = 6 if intent["before"] == 3 else 2
                assert not q["home"] and r["opcode"] == 9 and r["resp"] == expected_resp and r["forward_state"] == fwd
                assert r["trace_tag"] == intent["trace_tag"] and r["resp_err"] == 0
                q["home"] = True; retire(key)
            else: raise AssertionError("unmodeled channel in bounded forward fixture")
    assert next(requests,None) is None and not active and not dbids and not logged_ack and not logged_data
    return dict(obligations=len(completed), peakLiveObligations=peak, observations=len(observations),
                completions=completed,
                scope="fixed DUT1/forward12/Home8, two concurrent stable-line UniqueFwd obligations and identity reuse; no arbitrary topology")


def mutations(events, observations, intents):
    variants=[]
    for i,r in enumerate(observations):
        for field,value in (("cycle",r["cycle"]+1),("txn",r["txn"]+1),("dbid",r["dbid"]+1),
                            ("target",1),("trace_tag",not r["trace_tag"])):
            changed=copy.deepcopy(observations); changed[i][field]=value
            variants.append((events,changed,intents))
        changed=copy.deepcopy(observations); changed.pop(i); variants.append((events,changed,intents))
    for i,r in enumerate(events):
        if r["direction"] == "chi_tx_dat":
            for field,value in (("txn_id",r["txn_id"]+1),("dbid",r["dbid"]+1),("data_id",1),
                                ("node",2),("target",8),("trace_tag",not r["trace_tag"])):
                changed=copy.deepcopy(events); changed[i][field]=value
                variants.append((changed,observations,intents))
            changed=copy.deepcopy(events); payload=bytearray.fromhex(changed[i]["data"]); payload[0]^=1
            changed[i]["data"]=payload.hex(); variants.append((changed,observations,intents))
        elif r["direction"] == "chi_tx_rsp":
            changed=copy.deepcopy(events); changed.pop(i); variants.append((changed,observations,intents))
    for canonical,rows,inputs in variants:
        try: check(canonical,rows,inputs)
        except (AssertionError,KeyError,StopIteration,ValueError): pass
        else: raise AssertionError("multi-forward checker accepted an identity/data/retirement mutation")
    return len(variants)
