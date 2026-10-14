"""Host negatives for random-segment completion and intent qualification."""
import copy
from pathlib import Path
import runpy
import tempfile

tools = runpy.run_path(str(Path(__file__).with_name("check-random-actions.py")))
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / "cchitest.ini").write_text("[tltest.config]\nmemory.start=4096\nmmio.start=262144\n[tltest.fuzzer]\nrandom.episodes=3\nrandom.lines=4\n")
    actions = []
    for node, address, kinds in ((0,4096,(1,11,5,3)), (1,4160,(8,)), (4,262144,(10,9))):
        for k in kinds:
            index = len(actions)
            actions.append(dict(cycle=10+index,kind=k,node=node,address=address,txn=20+index,
                trace_tag=index%2==0,size=3 if k in (9,10) else 6,byte_enable=255))
    fields = ("mshrs", "responses", "writebacks", "requests", "queues", "pipeline", "serializers", "controllers", "retryOwners", "credits")
    resources = [dict(cycle=a["cycle"],ready=True,**{field:0 for field in fields}) for a in actions]
    assert tools["check"](root,actions,resources)["segments"] == 3
    (root / "cchitest.ini").write_text("[tltest.config]\nmemory.start=0x1000\nmmio.start=0x40000\n[tltest.fuzzer]\nrandom.episodes=0x3\nrandom.lines=0x4\n")
    assert tools["check"](root,actions,resources)["segments"] == 3
    variants = []
    changed=copy.deepcopy(actions); changed.pop(); variants.append(changed)
    for index,field,value in ((1,"kind",8),(1,"node",2),(1,"address",4224),
                              (5,"byte_enable",0),(0,"txn",99),(0,"trace_tag",False)):
        changed=copy.deepcopy(actions); changed[index][field]=value; variants.append(changed)
    for changed in variants:
        try: tools["check"](root,changed,resources)
        except (AssertionError,KeyError,IndexError): pass
        else: raise AssertionError("random segment qualification accepted mutated intent/budget")
    busy=copy.deepcopy(resources); busy[0]["pipeline"]=1
    try: tools["check"](root,actions,busy)
    except AssertionError: pass
    else: raise AssertionError("serialized action qualification accepted live resources")
    print("Random action checker: completed budget, legal client/line segments, zero starting resources and 8 negatives passed")
