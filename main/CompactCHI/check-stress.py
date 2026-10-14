"""Independent pressure/endpoint completion metrics over real accepted wire traffic."""
import configparser
from collections import Counter, defaultdict, deque
import json
import shutil
import tempfile
from pathlib import Path


def check(root, events, actions, resources):
    ini = configparser.ConfigParser(); ini.read(root / 'tltest.ini')
    cfg = ini['tltest.stress']; pattern = cfg['pattern']
    cycles = int(cfg['cycles']); window = int(cfg['outstanding'])
    result = json.loads((root / 'native-result.json').read_text())
    assert result['state'] == 'finished' and result['stimulusComplete'] and not result['timeout']
    assert result['cycle'] >= 8 + cycles, 'stimulus ended before the configured pressure budget'
    assert len(actions) >= 128, 'no meaningful stress traffic'
    assert actions[-1]['cycle'] - actions[0]['cycle'] >= cycles // 2, 'traffic was a short burst, not sustained'
    intents = defaultdict(deque)
    for a in actions:
        if a['kind'] != 11: intents[a['node'],a['txn']].append(a)
    live, acknowledgements, writes = {}, {}, {}
    peaks, per_node, overlapping, retired = Counter(), Counter(), 0, 0
    lower_overlap = 0
    def retire(key):
        nonlocal retired
        per_node[key[0]] -= 1; del live[key]; retired += 1
    stalls = Counter()
    for r in events:
        d, op = r['direction'], r['opcode']
        if r['valid'] and not r['ready']: stalls[d] += 1
        if not r['handshake']: continue
        if d == 'chi_rx_snp': lower_overlap += bool(live)
        if d in ('cchi_rx_req','cchi_rx_evt'):
            key = r['node'],r['txn_id']
            assert key not in live and intents[key]
            a = intents[key].popleft()
            assert a['address'] == r['address']
            overlapping += bool(live)
            live[key] = dict(action=a,beats=set(),completion=False)
            per_node[key[0]] += 1
            peaks[key[0]] = max(peaks[key[0]],per_node[key[0]])
        elif d == 'cchi_tx_dat':
            key = r['target'],r['txn_id']; q = live[key]; a=q['action']
            q['beats'].add(r['data_id'])
            if a['kind'] in (0,1):
                acknowledgements[r['target'],r['dbid']] = key
            elif q['beats'] == ({0,1} if a['size']==6 else {(a['address']>>5)&1}): retire(key)
        elif d == 'cchi_tx_rsp':
            key=r['target'],r['txn_id']; q=live[key]; a=q['action']
            if op in (2,3):
                writes[r['target'],r['dbid']] = key
                q['completion'] = op==3
            elif a['kind']==2: acknowledgements[r['target'],r['dbid']] = key
            else: retire(key)
        elif d == 'cchi_rx_rsp' and op==0:
            key=acknowledgements.pop((r['node'],r['txn_id']))
            assert live[key]['action']['kind']==2 or live[key]['beats']=={0,1}
            retire(key)
        elif d == 'cchi_rx_dat' and op in (0,2):
            slot=r['node'],r['txn_id']; key=writes[slot]; q=live[key]; a=q['action']
            q['beats'].add(r['data_id'])
            required={0,1} if a['size']==6 else {(a['address']>>5)&1}
            if q['beats']==required:
                del writes[slot]
                if q['completion']: retire(key)
    assert not live and not acknowledgements and not writes and all(not q for q in intents.values())
    assert retired == sum(a['kind']!=11 for a in actions)
    assert max(peaks.values(),default=0) >= 2, 'no actual per-node multiple outstanding transactions'
    assert max(peaks.values()) <= window, 'configured per-node wire window exceeded'
    assert overlapping >= 64, 'scheduler kept draining between requests'
    maxima={k:max(r[k] for r in resources) for k in ('mshrs','responses','writebacks','requests','queues','retryOwners','credits')}
    assert maxima['mshrs'] >= (16 if pattern=='stream' else 4), 'MSHR pressure was not exercised'
    assert stalls['cchi_tx_dat'] > 0 and stalls['chi_tx_req'] > 0, 'required upstream/downstream stalls absent'
    fired=[r for r in events if r['handshake']]
    assert any(r['direction']=='chi_rx_rsp' and r['opcode']==3 for r in fired), 'Retry boundary absent'
    assert any(r['direction']=='chi_rx_rsp' and r['opcode']==7 for r in fired), 'P-Credit boundary absent'
    topology=json.loads((root/'topology.json').read_text())
    assert {a['node'] for a in actions} == {n['id'] for n in topology['nodes'] if n['mode']!=0}, 'configured active upstream node was not exercised'
    if pattern=='same-set':
        assert sum(r['direction']=='chi_tx_req' and r['opcode']==0x42 for r in fired) >= 4, 'same-set replacement boundary absent'
    if pattern=='mixed':
        assert maxima['writebacks'] > 0
        assert {4,5,6,7,9,10,11} <= {a['kind'] for a in actions}, 'mixed writeback/CMO/MMIO/store paths missing'
        assert sum(r['direction']=='chi_rx_snp' for r in fired) == 2, 'lower-Snoop nodes not exercised'
        assert lower_overlap > 0, 'lower Snoop never overlapped live upstream work'
        assert any(r['direction']=='chi_tx_dat' and r['target']==12 for r in fired), 'forward endpoint absent'
    return dict(pattern=pattern,stimulusCycles=cycles,acceptedActions=len(actions),completedWireTransactions=retired,
                overlappingAdmissions=overlapping,lowerSnoopOverlap=lower_overlap,perNodePeakOutstanding=dict(peaks),resourceMaxima=maxima,
                stalledCycles=dict(stalls),scope='sustained legal per-line workers, default geometry and fixed Home/forward endpoints')


def mutations(root, events, actions, resources):
    # Exercise false-PASS paths against real accepted traffic, with metadata in
    # a temporary directory. The accepted native evidence is never rewritten.
    rejected = 0
    with tempfile.TemporaryDirectory() as directory:
        copied = Path(directory)
        for name in ('tltest.ini','native-result.json','topology.json'):
            shutil.copy2(root/name,copied/name)
        baseline = (copied/'native-result.json').read_text()
        variants = [
            (events, actions[:1], resources),
            (events, actions, [dict(r,mshrs=0) for r in resources]),
            ([dict(r,handshake=False) if r['direction']=='chi_rx_rsp' and r['opcode']==7 else r for r in events], actions, resources),
        ]
        for es, acts, rs in variants:
            try: check(copied,es,acts,rs)
            except (AssertionError,KeyError,ValueError): rejected += 1
            else: raise AssertionError('pressure checker accepted incomplete/idle/no-credit traffic')
        failed = json.loads(baseline); failed['timeout'] = True
        (copied/'native-result.json').write_text(json.dumps(failed))
        try: check(copied,events,actions,resources)
        except AssertionError: rejected += 1
        else: raise AssertionError('pressure checker accepted a timed-out run')
    return rejected
