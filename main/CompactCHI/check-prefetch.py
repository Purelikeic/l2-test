"""Independent integer PBOP teacher replay from accepted s3 training intent.

Table decisions and predicted addresses never come from DUT score/offset/RR
observations. Terminal bytes stay with the existing independent Home replay.
"""
import copy
from collections import Counter, deque

OFFSETS = (-32,-30,-27,-25,-24,-20,-18,-16,-15,-12,-10,-9,-8,-6,-5,-4,-3,-2,-1,
           1,2,3,4,5,6,8,9,10,12,15,16,18,20,24,25,27,30)


class Teacher:
    def __init__(self):
        self.cycle = 0
        self.recent, self.delayed, self.replies = {}, deque(), deque()
        self.scores = [0]*len(OFFSETS)
        self.learning = False
        self.pointer = self.round = self.best_score = 0
        self.best_offset = self.offset = 2
        self.disabled = False
        self.phases = self.dropped = 0

    @staticmethod
    def mapping(address):
        line = (address & ((1 << 48)-1)) >> 6
        return (line & 255) ^ ((line >> 8) & 255), (line >> 8) & 4095

    def step(self, address=None):
        old_learning = self.learning
        assert address is None or old_learning, "training accepted during teacher restart"
        reply = self.replies.popleft() if self.replies and self.replies[0][0] == self.cycle else None
        query = None
        if address is not None:
            index, tag = self.mapping(address-OFFSETS[self.pointer]*64)
            query = self.pointer, self.recent.get(index) == tag
        full = len(self.delayed) == 16
        if query is None and self.delayed and self.delayed[0][1] <= self.cycle:
            delayed, _ = self.delayed.popleft()
            index, tag = self.mapping(delayed)
            self.recent[index] = tag
        if address is not None:
            if full: self.dropped += 1
            else: self.delayed.append((address,self.cycle+301))
            self.replies.append((self.cycle+2,*query))
        if not old_learning:
            self.scores = [0]*len(OFFSETS)
            self.pointer = self.round = 0
            self.offset = self.best_offset
            self.disabled = self.best_score < 1
            self.best_score = 0
            self.learning = True
        else:
            finish_round = self.round >= 50
            if address is not None:
                if self.pointer == len(OFFSETS)-1: self.pointer=0; self.round+=1
                else: self.pointer+=1
            finish_score = False
            if reply is not None and reply[2]:
                index = reply[1]
                self.scores[index] = min(31,self.scores[index]+1)
                if self.scores[index] > self.best_score:
                    self.best_score = self.scores[index]
                    self.best_offset = OFFSETS[index]
                finish_score = self.scores[index] >= 31
            if finish_round or finish_score:
                self.learning = False
                self.phases += 1
        self.cycle += 1


def check(samples, events, require_unique_read_once_coverage=False):
    assert samples and samples[0]["cycle"] == 8
    teacher, candidates, pending = Teacher(), deque(), Counter()
    requests = [r for r in events if r["handshake"] and r["direction"] == "cchi_rx_req" and r["opcode"] in (3,4,16)]
    used = set()
    trained = issued = completed = dropped = cross_page = 0
    for index,row in enumerate(samples):
        assert row["cycle"] == samples[0]["cycle"]+index and row["sim_time"] == row["cycle"]*2
        assert not (row["issue"] is not None and row["drop"] is not None), "one prefetch cannot be issued and dropped in the same cycle"
        for name in ("issue","drop"):
            record = row[name]
            if record is None: continue
            assert record["source"] == 2, "unexpected prefetch source in the PBOP-only profile"
            assert candidates, "prefetch has no independent teacher prediction"
            expected, train_cycle = candidates.popleft()
            assert row["cycle"] > train_cycle and record == expected, "prefetch address/source/permission/tag differs from teacher"
            if name == "issue":
                pending[record["address"],record["source"],record["traceTag"]] += 1
                issued += 1
            else: dropped += 1
        record = row["complete"]
        if record is not None:
            key = record["address"], record["source"], record["traceTag"]
            assert pending[key] > 0, "prefetch feedback lost or changed its accepted request identity"
            assert not record["denied"], "normal-profile prefetch was denied"
            pending[key] -= 1
            completed += 1
        train = row["train"]
        address = None
        if train is not None:
            assert not train["hit"] or train["prefetched"]
            address = train["address"]
            owner = next((i for i,r in enumerate(requests) if i not in used and r["node"] == train["source"] and
                          r["address"] == address and int(r["trace_tag"]) == train["traceTag"] and r["cycle"]+3 <= row["cycle"]),None)
            assert owner is not None, "s3 training lacks an accepted demand intent"
            used.add(owner)
            assert train["needT"] == (requests[owner]["opcode"] == 16)
            trained += 1
            if not teacher.disabled:
                predicted = (address+teacher.offset*64) & ((1<<48)-1)
                if address >> 12 == predicted >> 12:
                    candidates.append((dict(address=predicted,source=2,unique=train["needT"],traceTag=train["traceTag"]),row["cycle"]))
                else: cross_page += 1
        assert row["delayedTrainingDropped"] == (address is not None and len(teacher.delayed) == 16), "delay training drop differs from independent queue model"
        teacher.step(address)
    assert not candidates and not any(pending.values()), "terminal prefetch predictions/feedback remain live"
    coverage = "observed-training-only; eligibility completeness is not claimed"
    if require_unique_read_once_coverage:
        fired = [r for r in events if r["handshake"]]
        assert all(r["opcode"] == 3 for r in fired if r["direction"] == "cchi_rx_req"), "coverage proof requires ReadOnce-only stimulus"
        assert len({r["address"] for r in requests}) == len(requests), "coverage proof requires unique first-access addresses"
        assert not any(r["direction"] in ("cchi_rx_evt", "chi_rx_snp") for r in fired), "maintenance/Snoop overlap is outside the coverage proof"
        assert all(r.get("resp_err") in (None, 0) for r in fired), "error training eligibility is outside the coverage proof"
        assert not any(r["trainingDropped"] for r in samples), "cannot claim exact coverage when s3 snapshots were dropped"
        # Empty reset, unique first-demand addresses and PBOP-only allocation:
        # every demand is a miss or first use of a prefetched line. This
        # expectation comes from stimulus, not the DUT's hit/prefetched flags.
        assert used == set(range(len(requests))), "an independently eligible ReadOnce demand was not trained"
        coverage = "exact coverage of unique first-access ReadOnce with no s3 drops or maintenance/errors; other qualifications remain unproved"
    return dict(trainingSamples=trained,issued=issued,completed=completed,dropped=dropped,
                pageCrossingsSuppressed=cross_page,learningPhases=teacher.phases,
                delayedTrainingDropped=teacher.dropped,s3TrainingDropped=sum(r["trainingDropped"].bit_count() for r in samples),
                trainingCoverage=coverage,
                scope="PBOP learning/output replay from observed accepted training, matched to demand intent; optional bounded stimulus-derived coverage; bytes checked by independent storage replay")


def mutations(samples, events, require_unique_read_once_coverage=False):
    variants=[]
    for name in ("train","issue","drop","complete"):
        index=next((i for i,r in enumerate(samples) if r[name] is not None),None)
        if index is None: continue
        for field,value in (("address",samples[index][name]["address"]+64),("source",15),("traceTag",1-samples[index][name]["traceTag"])):
            changed=copy.deepcopy(samples); changed[index][name][field]=value; variants.append(changed)
        if name in ("issue","complete") or (name == "train" and require_unique_read_once_coverage):
            changed=copy.deepcopy(samples); changed[index][name]=None; variants.append(changed)
    for changed in variants:
        try: check(changed,events,require_unique_read_once_coverage)
        except (AssertionError,KeyError,ValueError): pass
        else: raise AssertionError("prefetch checker accepted an intent/address/source/feedback mutation")
    return len(variants)
