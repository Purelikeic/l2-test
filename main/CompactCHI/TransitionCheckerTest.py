"""Bounded generated-transition host fixtures, independent of DUT observations."""
import copy
import json
from pathlib import Path
import runpy
import sys

HERE = Path(__file__).resolve().parent
architectural_check = runpy.run_path(str(HERE / "check-architectural-state.py"))["check"]
transition_tools = runpy.run_path(str(HERE / "check-transitions.py"))
data_tools = runpy.run_path(str(HERE / "check-datastorage.py"))
directory_tools = runpy.run_path(str(HERE / "check-directory.py"))
root = Path(sys.argv[1]); root.mkdir(parents=True, exist_ok=True)
ADDRESS, SEED = 4096, 42
OLD = bytes(((ADDRESS >> 6) ^ (i * 17) ^ SEED) & 255 for i in range(64))
NEW = bytes(b ^ 0xa5 for b in OLD)
TOPO = dict(nodes=[dict(id=0, kind=0)])


class Program:
    def __init__(self):
        self.events, self.actions = [], []
        self.cycle, self.txn = 10, 20
        self.latest = self.memory = OLD
        self.copy = None
        self.stored = None
        self.state = 0

    def emit(self, direction, node, target, txn, opcode, **fields):
        self.cycle += 1
        r = dict(cycle=self.cycle, direction=direction, node=node, target=target,
                 txn_id=txn, opcode=opcode, address=None, data_id=None, dbid=None,
                 trace_tag=True, handshake=True, resp=0, resp_err=0, byte_enable=0xffffffff)
        r.update(fields); self.events.append(r)
        return r

    def action(self, kind, data=None):
        self.cycle += 1; self.txn += 1
        self.actions.append(dict(cycle=self.cycle, kind=kind, node=0, txn=self.txn,
                                 address=ADDRESS, trace_tag=True, size=6,
                                 data=(data if data is not None else bytes(64)).hex(), byte_enable=(1 << 64)-1))
        if kind != 11:
            opcode = {0: 4, 1: 16, 2: 18, 3: 0, 4: 1, 5: 12, 6: 13, 7: 14, 8: 3}[kind]
            self.emit("cchi_rx_evt" if kind in (3, 4) else "cchi_rx_req", 0, 1, self.txn, opcode, address=ADDRESS)
        return self.txn

    def read(self, kind=8):
        txn = self.action(kind)
        self.emit("chi_tx_req", 1, 8, 7, 7 if kind == 1 else 38, address=ADDRESS)
        for beat in (0, 1):
            self.emit("chi_rx_dat", 8, 1, 7, 4, data_id=beat*2, dbid=71, resp=2,
                      data=self.memory[beat*32:(beat+1)*32].hex())
        permission = {0: 1, 1: 2, 8: 0}[kind]
        for beat in (0, 1):
            self.emit("cchi_tx_dat", 1, 0, txn, 0, data_id=beat, dbid=72, resp=permission,
                      data=self.memory[beat*32:(beat+1)*32].hex())
        if kind != 8:
            self.emit("cchi_rx_rsp", 0, 1, 72, 0)
            self.copy = (permission, self.memory)
        self.stored, self.state = self.memory, 2 if kind == 1 else 1

    def store(self):
        self.action(11, NEW)
        self.latest = NEW; self.copy = (3, NEW)

    def makeunique(self):
        txn = self.action(2, NEW)
        self.emit("chi_tx_req", 1, 8, 7, 12, address=ADDRESS)
        self.emit("chi_rx_rsp", 8, 1, 7, 4)
        self.emit("cchi_tx_rsp", 1, 0, txn, 1, dbid=72, resp=2)
        self.emit("cchi_rx_rsp", 0, 1, 72, 0)
        self.latest = NEW; self.copy = (3, NEW)
        self.stored = None; self.state = 2

    def writeback(self):
        txn = self.action(4)
        self.emit("cchi_tx_rsp", 1, 0, txn, 3, dbid=73)
        for beat in (0, 1):
            self.emit("cchi_rx_dat", 0, 1, 73, 2, data_id=beat, resp=6,
                      data=self.latest[beat*32:(beat+1)*32].hex())
        self.copy = None; self.stored = self.latest; self.state = 3

    def victim(self, dirty=False, data_branch=False):
        if self.copy:
            self.emit("cchi_tx_snp", 1, 0, 80, 1, address=ADDRESS)
            for beat in (0, 1):
                self.emit("cchi_rx_dat", 0, 1, 80, 3, data_id=beat, resp=4,
                          data=self.latest[beat*32:(beat+1)*32].hex())
            self.copy = None
        opcode = 23 if dirty else 66 if data_branch else 13
        self.emit("chi_tx_req", 1, 8, 7, opcode, address=ADDRESS)
        self.emit("chi_rx_rsp", 8, 1, 7, 5 if dirty or data_branch else 4, dbid=74)
        if dirty or data_branch:
            for beat in (0, 1):
                self.emit("chi_tx_dat", 1, 8, 74, 2, data_id=beat*2,
                          data=self.latest[beat*32:(beat+1)*32].hex())
            self.memory = self.latest
        self.stored = None; self.state = 0

    def cmo(self, kind, upper_first=False):
        txn = self.action(kind)
        if self.copy:
            self.emit("cchi_tx_snp", 1, 0, 80, {5: 2, 6: 1, 7: 0}[kind], address=ADDRESS)
            if kind == 5:
                for beat in ((1, 0) if upper_first else (0, 1)):
                    self.emit("cchi_rx_dat", 0, 1, 80, 3, data_id=beat, resp=5,
                              data=self.latest[beat*32:(beat+1)*32].hex())
                self.copy = (1, self.latest); self.stored = self.latest
            else:
                self.emit("cchi_rx_rsp", 0, 1, 80, 1)
                self.copy = None
        if kind == 5:
            self.emit("chi_tx_req", 1, 8, 7, 23, address=ADDRESS)
            self.emit("chi_rx_rsp", 8, 1, 7, 5, dbid=74)
            for beat in (0, 1):
                self.emit("chi_tx_dat", 1, 8, 74, 2, data_id=beat*2,
                          data=self.latest[beat*32:(beat+1)*32].hex())
            self.memory = self.latest
        elif kind == 6:
            self.emit("chi_tx_req", 1, 8, 7, 13, address=ADDRESS)
            self.emit("chi_rx_rsp", 8, 1, 7, 4)
        self.emit("chi_tx_req", 1, 8, 7, {5: 16, 6: 17, 7: 15}[kind], address=ADDRESS)
        self.emit("chi_rx_rsp", 8, 1, 7, 4)
        self.emit("cchi_tx_rsp", 1, 0, txn, 4)
        if kind == 5: self.state = 2
        else: self.state = 0; self.stored = None
        if kind == 7: self.latest = self.memory

    def snapshot(self):
        geometry = dict(schema=1, sets=2, ways=2, slices=4, lineBytes=64, addrBits=48)
        directory, data = [geometry], [geometry]
        start = max(30, self.cycle + 21)
        for i in range(4):
            entries = [dict(tag=0, state=0, client=False, clientState=0, clientID=0) for _ in range(4)]
            lines = [dict(data=bytes(64).hex(), error=False) for _ in range(4)]
            if i == 0:
                entries[0].update(tag=8, state=self.state, client=self.copy is not None,
                                  clientState=1 if self.copy and self.copy[0] == 1 else 2 if self.copy else 0)
                # Explicit undefined storage deliberately differs from latest.
                lines[0]["data"] = (self.stored if self.stored is not None else bytes([0x3c])*64).hex()
            directory.append(dict(cycle=start+i, sim_time=2*(start+i), set=i//2, way=i%2, entries=entries))
            data.append(dict(cycle=start+4+i, sim_time=2*(start+4+i), set=i//2, way=i%2, lines=lines))
        resources = [dict(cycle=c, sim_time=2*c, ready=True, **{f: 0 for f in data_tools["FIELDS"]}) for c in range(10, start+8)]
        expected = dict(requesters=[dict(node=0, address=ADDRESS, permission=self.copy[0], data=self.copy[1].hex())] if self.copy else [],
                        memory=[dict(address=ADDRESS, data=self.memory.hex())], devices=[])
        terminal = dict(schema=1, cycle=start+7, expected=expected, observed=copy.deepcopy(expected))
        return terminal, directory, data, resources


cases = ("upstream-writeback", "makeunique-no-data", "cleanshared", "cleanshared-upper-first",
         "cleaninvalid", "cleaninvalid-refill", "makeinvalid", "makeinvalid-refill",
         "victim-writeback", "victim-writeback-refill", "victim-evict", "victim-evict-refill",
         "victim-clean-data", "victim-clean-data-refill", "victim-dirty-recall", "victim-dirty-recall-refill")
results, sources, rejections, order_rejections = [], set(), 0, 0
for name in cases:
    program = Program()
    if name.startswith("victim") or name == "upstream-writeback":
        program.read(1 if "writeback" in name or "dirty-recall" in name else 8)
        if "writeback" in name or "dirty-recall" in name:
            program.store()
            if "writeback" in name: program.writeback()
        if name.startswith("victim"):
            program.victim(dirty="writeback" in name or "dirty-recall" in name, data_branch="clean-data" in name)
    else:
        program.makeunique()
        if name.startswith("cleanshared") or name.startswith("cleaninvalid"):
            program.cmo(5, upper_first=name.endswith("upper-first"))
        if name.startswith("cleaninvalid"): program.cmo(6)
        if name.startswith("makeinvalid"): program.cmo(7)
    if name.endswith("refill"): program.read()
    terminal, directory, data, resources = program.snapshot()
    (root / "actions.jsonl").write_text("".join(json.dumps(a)+"\n" for a in program.actions))
    (root / "rejections.jsonl").write_text("")
    architectural = architectural_check(root, program.events, SEED, TOPO, terminal)
    evidence = transition_tools["check"](program.events, program.actions, architectural, directory)
    directory_tools["check"](directory, resources, {(v["node"], v["address"]): v["clientState"] for v in architectural["requesterFootprint"]}, {0: 0}, data[-1]["cycle"])
    storage = data_tools["check"](directory, data, resources, architectural["l2DataVersions"], architectural["l2UndefinedStorage"], architectural["l2Transitions"])
    mutation = transition_tools["mutations"](program.events, program.actions, architectural, directory)
    rejections += mutation["rejections"]; sources.update(mutation["sources"])
    # Illegal raw canonical order, before the independent replay emits a ledger.
    for direction, opcode in (("cchi_rx_dat", 2), ("cchi_rx_dat", 3), ("chi_tx_dat", 2), ("chi_rx_dat", 4)):
        candidates = [r for r in program.events if r["direction"] == direction and r["opcode"] == opcode]
        if not candidates: continue
        for variant in ("missing-beat", "data-before-request", "wrong-txn", "wrong-bytes"):
            changed = copy.deepcopy(program.events)
            victim = next(r for r in changed if r["direction"] == direction and r["opcode"] == opcode)
            if variant == "missing-beat": victim["handshake"] = False
            elif variant == "data-before-request":
                victim["cycle"] = program.events[0]["cycle"] - 1
                changed.sort(key=lambda r: r["cycle"])
            elif variant == "wrong-txn": victim["txn_id"] += 1
            else:
                payload = bytearray.fromhex(victim["data"]); payload[0] ^= 1; victim["data"] = payload.hex()
            try:
                changed_arch = architectural_check(root, changed, SEED, TOPO, terminal)
                transition_tools["check"](changed, program.actions, changed_arch, directory)
            except (AssertionError, KeyError, IndexError): order_rejections += 1
            else: raise AssertionError(name + " accepted " + direction + " " + variant)
    make_request = next((r for r in program.events if r["direction"] == "chi_tx_req" and r["opcode"] == 12), None)
    if make_request:
        changed = copy.deepcopy(program.events)
        home_comp = next(r for r in changed if r["direction"] == "chi_rx_rsp" and r["opcode"] == 4 and r["cycle"] > make_request["cycle"])
        home_comp["handshake"] = False
        try:
            changed_arch = architectural_check(root, changed, SEED, TOPO, terminal)
            transition_tools["check"](changed, program.actions, changed_arch, directory)
        except (AssertionError, KeyError, IndexError): order_rejections += 1
        else: raise AssertionError(name + " accepted MakeUnique grant without Home completion")
    cmo_completion = next((r for r in program.events if r["direction"] == "cchi_tx_rsp" and r["opcode"] == 4), None)
    if cmo_completion:
        changed = copy.deepcopy(program.events)
        completion = next(r for r in changed if r["direction"] == "cchi_tx_rsp" and r["opcode"] == 4)
        nested = next(r for r in changed if r["direction"] == "cchi_tx_snp")
        completion["cycle"] = nested["cycle"]
        changed.sort(key=lambda r: r["cycle"])
        try: architectural_check(root, changed, SEED, TOPO, terminal)
        except (AssertionError, KeyError, IndexError): order_rejections += 1
        else: raise AssertionError(name + " accepted CMO before nested Snoop retirement")
    results.append(dict(case=name, transitionEvidence=evidence, dataStorageEvidence=storage, mutations=mutation))
state = dict(state="passed", cases=len(results), mutationRejections=rejections, rawCanonicalRejections=order_rejections,
             sources=sorted(sources), results=results,
             scope="generated host transition combinations; no DUT or all-combinations acceptance")
(root / "transition-host.json").write_text(json.dumps(state, indent=2)+"\n")
print("Transition host checker:", len(results), "cases,", rejections, "source/state mutations and", order_rejections, "raw order/identity/data mutations passed")
