"""Independent small terminal residency fixtures; not DUT replacement evidence."""
import copy
import json
from pathlib import Path
import runpy
import sys

HERE=Path(__file__).resolve().parent
tools=runpy.run_path(str(HERE/"check-residency.py"))
data_tools=runpy.run_path(str(HERE/"check-datastorage.py"))
root=Path(sys.argv[1]); root.mkdir(parents=True,exist_ok=True)
G=dict(schema=1,sets=2,ways=2,slices=4,lineBytes=64,addrBits=48)
A=4096
OLD=bytes(((A>>6)^(i*17)^42)&255 for i in range(64))
NEW=bytes(v^0xa5 for v in OLD)


class Fixture:
    def __init__(self):
        self.events=[]; self.actions=[]; self.records=[]; self.sequence=["initial"]
        self.stored=None; self.client=False; self.client_state=0; self.expected_state=0; self.undefined=False
    def emit(self,direction,opcode,**fields):
        r=dict(cycle=len(self.events)+10,direction=direction,opcode=opcode,handshake=True,
               node=1,target=8,txn_id=7,address=None,resp=0,dbid=10,data_id=None)
        r.update(fields); self.events.append(r); return len(self.events)-1
    def intent(self,kind,opcode,direction="cchi_rx_req"):
        txn=20+len(self.actions)
        self.actions.append(dict(kind=kind,node=0,txn=txn,address=A,cycle=len(self.events)+9))
        return self.emit(direction,opcode,node=0,target=1,txn_id=txn,address=A)
    def record(self,kind,index,origin):
        self.sequence.append(kind)
        self.records.append(dict(kind=kind,address=A,eventIndex=index,originIndex=origin))
    def read(self,kind=8):
        origin=self.intent(kind,{0:4,1:16,8:3}[kind])
        req=self.emit("chi_tx_req",7 if kind==1 else 38,address=A)
        self.emit("chi_rx_dat",4,node=8,target=1,data_id=0,resp=2 if kind==1 else 1)
        end=self.emit("chi_rx_dat",4,node=8,target=1,data_id=2,resp=2 if kind==1 else 1)
        self.record("refill",end,req); self.stored=OLD
        txn=self.events[origin]["txn_id"]
        for beat in (0,1): self.emit("cchi_tx_dat",0,target=0,txn_id=txn,data_id=beat)
        if kind!=8: self.emit("cchi_rx_rsp",0,node=0,target=1,txn_id=10)
        self.client=kind!=8; self.client_state=1 if kind==0 else 2 if kind==1 else 0
        self.expected_state=2 if kind==1 else 1
    def makeunique(self):
        origin=self.intent(2,18)
        self.emit("chi_tx_req",12,address=A)
        self.emit("chi_rx_rsp",4,node=8,target=1,resp=2)
        self.emit("cchi_tx_rsp",1,target=0,txn_id=self.events[origin]["txn_id"],resp=2)
        done=self.emit("cchi_rx_rsp",0,node=0,target=1,txn_id=10)
        self.record("makeunique-no-data",done,origin)
        self.undefined=True; self.client=True; self.client_state=2; self.expected_state=2
    def writeback(self):
        self.actions.append(dict(kind=11,node=0,txn=99,address=A,cycle=len(self.events)+10,data=NEW.hex()))
        origin=self.intent(4,1,"cchi_rx_evt")
        self.emit("cchi_tx_rsp",3,target=0,txn_id=self.events[origin]["txn_id"],dbid=16)
        self.emit("cchi_rx_dat",2,node=0,target=1,txn_id=16,data_id=0)
        end=self.emit("cchi_rx_dat",2,node=0,target=1,txn_id=16,data_id=1)
        self.record("upstream-writeback-install",end,origin)
        self.stored=NEW; self.client=False; self.client_state=0; self.expected_state=3
    def evict(self):
        origin=self.intent(3,0,"cchi_rx_evt")
        end=self.emit("cchi_tx_rsp",1,target=0,txn_id=self.events[origin]["txn_id"])
        self.record("evict-client-release",end,origin); self.client=False; self.client_state=0
    def cmo(self,kind):
        origin=self.intent(kind,{5:12,6:13,7:14}[kind])
        if kind in (5,6) and self.expected_state==3:
            req=self.emit("chi_tx_req",23,address=A)
            self.emit("chi_rx_rsp",5,node=8,target=1,dbid=30)
            self.emit("chi_tx_dat",2,txn_id=30,data_id=0)
            done=self.emit("chi_tx_dat",2,txn_id=30,data_id=2)
            self.record("cmo-writeback-commit",done,req)
        self.emit("chi_tx_req",{5:16,6:17,7:15}[kind],address=A)
        self.emit("chi_rx_rsp",4,node=8,target=1)
        end=self.emit("cchi_tx_rsp",4,target=0,txn_id=self.events[origin]["txn_id"])
        token={5:"cmo-cleanshared-preserve",6:"cmo-cleaninvalid-discard",7:"cmo-makeinvalid-discard"}[kind]
        self.record(token,end,origin)
        self.expected_state=(1 if self.expected_state==1 else 2) if kind==5 else 0
        if kind!=5: self.stored=None; self.client=False; self.client_state=0
    def lower(self,opcode):
        self.emit("chi_rx_snp",opcode,node=8,target=1,txn_id=80,address=A)
        if opcode==3:
            for beat in (0,2): self.emit("chi_tx_dat",1,txn_id=80,data_id=beat)
        else: self.emit("chi_tx_rsp",1,txn_id=80)
        if opcode in (1,17): self.expected_state=1
        elif opcode in (7,9,10,23): self.expected_state=0; self.stored=None
    def snapshot(self):
        directory,data=[G],[G]
        for i in range(4):
            entries=[dict(tag=0,state=0,client=False,clientState=0,clientID=0) for _ in range(4)]
            lines=[dict(data="00"*64,error=False) for _ in range(4)]
            if i==0:
                entries[0].update(tag=8,state=self.expected_state,client=self.client,clientState=self.client_state)
                lines[0]["data"]=(self.stored or bytes([0x3c])*64).hex()
            directory.append(dict(set=i//2,way=i%2,cycle=100+i,sim_time=2*(100+i),entries=entries))
            data.append(dict(set=i//2,way=i%2,cycle=104+i,sim_time=2*(104+i),lines=lines))
        versions=[dict(address=A,data=self.stored.hex())] if self.stored is not None else []
        arch=dict(l2TransitionEvents=self.records,l2DataVersions=versions,l2UndefinedStorage=[A] if self.undefined else [],
                  requesterFootprint=[dict(node=0,address=A,clientState=self.client_state)] if self.client else [])
        resources=[dict(cycle=c,sim_time=2*c,ready=True,**{k:0 for k in data_tools["FIELDS"]}) for c in range(80,108)]
        return directory,data,arch,resources


cases=("makeunique-no-data","readonce","shared-evict","unique-evict","writeback","dirty-cleanshared","clean-cleanshared",
       "cleaninvalid","makeinvalid","refill-after-discard","lower-shared","lower-invalidate","lower-once","lower-query")
results=[]; rejected=0
for name in cases:
    f=Fixture()
    if name=="makeunique-no-data": f.makeunique()
    else: f.read(0 if name=="shared-evict" else 1 if name in ("unique-evict","writeback","dirty-cleanshared","makeinvalid","refill-after-discard","lower-shared","lower-invalidate","lower-once","lower-query") else 8)
    if name in ("writeback","dirty-cleanshared","makeinvalid","refill-after-discard"): f.writeback()
    if name in ("shared-evict","unique-evict","lower-shared","lower-invalidate","lower-once","lower-query"): f.evict()
    if name in ("dirty-cleanshared","clean-cleanshared","refill-after-discard"): f.cmo(5)
    if name in ("cleaninvalid","refill-after-discard"): f.cmo(6)
    if name=="makeinvalid": f.cmo(7)
    if name=="refill-after-discard":
        # Refetched bytes are independently committed NEW after the prior clean.
        f.read(); f.stored=NEW
    if name.startswith("lower-"): f.lower({"lower-shared":1,"lower-invalidate":9,"lower-once":3,"lower-query":16}[name])
    directory,data,arch,resources=f.snapshot()
    model=tools["derive"](f.events,f.actions,arch,G)
    evidence=tools["check"](directory,data,model)
    rejected+=tools["mutations"](directory,data,model)
    data_tools["check"](directory,data,resources,arch["l2DataVersions"],arch["l2UndefinedStorage"],[dict(address=A,transitions=f.sequence)])
    results.append(dict(case=name,evidence=evidence))
    # Extra valid tag in an independently empty set/slice must be rejected.
    changed=copy.deepcopy(directory); changed[1]["entries"][1].update(tag=7,state=1)
    try: tools["check"](changed,data,model)
    except AssertionError: rejected+=1
    else: raise AssertionError("residency accepted an unallocated tag")
    if name in ("cleaninvalid","makeinvalid","lower-invalidate"):
        changed=copy.deepcopy(directory); changed[1]["entries"][0].update(tag=8,state=1)
        try: tools["check"](changed,data,model)
        except AssertionError: rejected+=1
        else: raise AssertionError("residency resurrected an invalidated tag")
    if name=="dirty-cleanshared":
        broken=copy.deepcopy(arch)
        broken["l2TransitionEvents"]=[t for t in broken["l2TransitionEvents"] if t["kind"]!="cmo-writeback-commit"]
        try: tools["derive"](f.events,f.actions,broken,G)
        except AssertionError: rejected+=1
        else: raise AssertionError("dirty cleaning lacked a completed writeback")

# Overflow without an independent vector stays explicitly unproved. The result
# is independent of which optional tags the observed snapshot happens to contain.
events=[]; records=[]; versions=[]
for address in (A,A+512,A+1024):
    origin=len(events)
    events.append(dict(cycle=origin+10,direction="chi_tx_req",opcode=38,node=1,target=8,txn_id=origin,address=address,handshake=True))
    events.append(dict(cycle=origin+11,direction="chi_rx_dat",opcode=4,node=8,target=1,txn_id=origin,address=None,resp=1,handshake=True))
    records.append(dict(kind="refill",address=address,eventIndex=origin+1,originIndex=origin))
    versions.append(dict(address=address,data=OLD.hex()))
arch=dict(l2TransitionEvents=records,l2DataVersions=versions,l2UndefinedStorage=[],requesterFootprint=[])
model=tools["derive"](events,[],arch,G)
assert len(model["exclusions"])==1 and not model["expected"]
results.append(dict(case="overflow-excluded",provedGroups=len(model["groups"]),exclusions=model["exclusions"]))

state=dict(state="passed",cases=len(results),mutationRejections=rejected,results=results,
           scope="independent synthetic terminal snapshots and input obligations; native evidence is checked separately")
(root/"residency-host.json").write_text(json.dumps(state,indent=2)+"\n")
print("Residency host checker:",len(results),"cases and",rejected,"mutations passed")
