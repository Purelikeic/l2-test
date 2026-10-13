"""Focused host regression for the observed-training/qualification distinction."""
import copy
from pathlib import Path
import runpy

tools = runpy.run_path(str(Path(__file__).with_name("check-prefetch.py")))
samples = [dict(cycle=c,sim_time=2*c,trainingDropped=0,delayedTrainingDropped=0,
                train=None,issue=None,drop=None,complete=None) for c in range(8,21)]
events = []
for txn,cycle,address in ((1,9,4096),(2,13,4160)):
    events.append(dict(cycle=cycle,direction="cchi_rx_req",handshake=True,node=0,
                       txn_id=txn,opcode=3,address=address,trace_tag=False))
    samples[cycle+3-8]["train"] = dict(address=address,source=0,needT=False,hit=False,prefetched=False,traceTag=0)
assert tools["check"](samples,events,True)["trainingSamples"] == 2
missing = copy.deepcopy(samples)
missing[16-8]["train"] = None
# The original learning replay deliberately cannot infer all missing eligibility.
assert tools["check"](missing,events)["trainingSamples"] == 1
rejected = 0
try: tools["check"](missing,events,True)
except AssertionError: rejected += 1
else: raise AssertionError("eligible first-access ReadOnce was omitted")
for variant in ("reused-address","maintenance","s3-drop"):
    rows,canonical = copy.deepcopy(samples),copy.deepcopy(events)
    if variant == "reused-address": canonical[1]["address"] = canonical[0]["address"]
    elif variant == "maintenance": canonical.append(dict(cycle=18,direction="chi_rx_snp",handshake=True,node=8,txn_id=7,opcode=1))
    else: rows[0]["trainingDropped"] = 1
    try: tools["check"](rows,canonical,True)
    except AssertionError: rejected += 1
    else: raise AssertionError("unproved training qualification was silently accepted")
conflicting = copy.deepcopy(samples)
conflicting[-1]["issue"] = conflicting[-1]["drop"] = dict(address=4096,source=2,unique=0,traceTag=0)
try: tools["check"](conflicting,events)
except AssertionError as error:
    assert "issued and dropped" in str(error)
    rejected += 1
else: raise AssertionError("issue/drop observation was not exclusive")
reused = copy.deepcopy(samples)
reused[12-8]["train"] = None
reused[16-8]["train"]["address"] = 4096
old_intents = copy.deepcopy(events)
old_intents[0]["opcode"] = 16
old_intents[1]["address"] = 4096
old_intents[1]["opcode"] = 4
assert tools["check"](reused,old_intents,serialized=True)["trainingSamples"] == 1
wrong_permission = copy.deepcopy(reused)
wrong_permission[16-8]["train"]["needT"] = True
try: tools["check"](wrong_permission,old_intents,serialized=True)
except AssertionError: rejected += 1
else: raise AssertionError("serialized matching hid changed needT behind an older request")
print("Prefetch checker boundary: observed-training limits, unique coverage, serialized reused-intent matching and",rejected,"negative cases passed")
