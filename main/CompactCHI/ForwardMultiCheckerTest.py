"""Host-only fixed-node forward concurrency and obligation retirement fixture."""
import copy
import json
from pathlib import Path
import runpy
import sys

root=Path(sys.argv[1])
tools=runpy.run_path(str(Path(__file__).with_name("check-forward-obligations.py")))
events=[json.loads(s) for s in (root/"canonical.jsonl").read_text().splitlines()][1:]
rows=[json.loads(s) for s in (root/"observations.jsonl").read_text().splitlines()]
intents=[json.loads(s) for s in (root/"intents.jsonl").read_text().splitlines()]
e=tools["check"](events,rows,intents)
assert e["obligations"]==3 and e["peakLiveObligations"]==2 and e["observations"]==9
rejected=tools["mutations"](events,rows,intents)
for variant in ("live-home-alias","live-forward-alias","same-line-overlap","external-ack-on-dut","early-reuse"):
    canonical,inputs=copy.deepcopy(events),copy.deepcopy(intents)
    if variant in ("live-home-alias","live-forward-alias","same-line-overlap"):
        field={"live-home-alias":"txn_id","live-forward-alias":"forward_txn","same-line-overlap":"address"}[variant]
        inputs[1][field]=inputs[0][field]
        next(r for r in canonical if r["direction"]=="chi_rx_snp" and r["txn_id"]==51)[field]=inputs[0][field]
    elif variant=="external-ack-on-dut":
        canonical.append(dict(cycle=14,direction="chi_rx_rsp",handshake=True,node=12,target=8,txn_id=50,opcode=2))
        canonical.sort(key=lambda r:r["cycle"])
    else:
        next(r for r in canonical if r["direction"]=="chi_rx_snp" and r["address"]==0x1080)["cycle"]=12
        canonical.sort(key=lambda r:r["cycle"])
    try: tools["check"](canonical,rows,inputs)
    except (AssertionError,KeyError,StopIteration,ValueError): rejected+=1
    else: raise AssertionError("multi-forward accepted "+variant)
state=dict(state="passed",evidence=e,mutationRejections=rejected,
           scope="host-only two concurrent stable UniqueFwd obligations, delayed Home join and identity reuse; fixed nodes 12/8")
(root/"forward-multi-host.json").write_text(json.dumps(state,indent=2)+"\n")
print("Multi-forward checker:",e["obligations"],"obligations, peak",e["peakLiveObligations"],"and",rejected,"mutations passed")
