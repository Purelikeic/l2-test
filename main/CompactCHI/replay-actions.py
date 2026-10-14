#!/usr/bin/env python3
"""Replay complete accepted action intent; cycles are observed, not forced."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HEADER = "ANSERL2_ACTIONS_V1"
FIELDS = ("node", "kind", "address", "txn", "trace_tag", "size", "byte_enable", "data")


def validate(action):
    limits = {"node": (1 << 32)-1, "kind": 11, "address": (1 << 64)-1,
              "txn": 255, "size": 6, "byte_enable": (1 << 64)-1}
    for name, maximum in limits.items():
        if type(action[name]) is not int or not 0 <= action[name] <= maximum:
            raise ValueError("invalid replay " + name)
    if type(action["trace_tag"]) is not bool:
        raise ValueError("invalid replay TraceTag")
    if action["address"] % (1 << action["size"]):
        raise ValueError("unaligned action replay address")
    data = action["data"]
    if not isinstance(data,str) or re.fullmatch(r"[0-9a-fA-F]{128}",data) is None:
        raise ValueError("replay needs complete 64-byte intent data")
    result = {name: action[name] for name in FIELDS}
    result["data"] = data.lower()
    return result


def encode(actions):
    if not 1 <= len(actions) <= 256:
        raise ValueError("action replay needs 1..256 accepted actions")
    rows = [HEADER]
    for action in actions:
        a = validate(action)
        rows.append(" ".join(str(int(a[k])) if k == "trace_tag" else str(a[k]) for k in FIELDS))
    return "\n".join(rows) + "\n"


def decode(text):
    rows = text.splitlines()
    if not rows or rows[0] != HEADER:
        raise ValueError("action replay version/header mismatch")
    actions=[]
    for row in rows[1:]:
        if not row: continue
        values=row.split()
        if len(values)!=8 or any(re.fullmatch(r"[0-9]+",v) is None for v in values[:7]):
            raise ValueError("invalid action replay fields")
        numbers=list(map(int,values[:7]))
        if numbers[4] not in (0,1): raise ValueError("invalid replay TraceTag")
        numbers[4]=bool(numbers[4])
        actions.append(validate(dict(zip(FIELDS,[*numbers,values[7]]))))
    if not 1 <= len(actions) <= 256: raise ValueError("action replay needs 1..256 accepted actions")
    return actions


def check(root, observed, resources):
    expected=decode((root / "replay-actions.txt").read_text())
    assert [validate(a) for a in observed] == expected, "replay dropped/reordered/changed accepted intent"
    by_cycle={r["cycle"]:r for r in resources}
    fields=("mshrs","responses","writebacks","requests","queues","pipeline",
            "serializers","controllers","retryOwners","credits")
    for a in observed:
        r=by_cycle[a["cycle"]]
        assert r["ready"] and all(r[k]==0 for k in fields), "action replay began with live resources"
    return dict(acceptedActions=len(observed),format=HEADER,completeIntentMatched=True,
                scope="serialized complete accepted intent replay; original cycle timing and unaccepted choices are not reproduced")


def classify_failure(text):
    match = re.search(r"Protocol cycle failed: ([^\n]+)",text)
    if match and not any(marker in match.group(1) for marker in
                         ("node rejected illegal stimulus action", "action selected an unconfigured node")):
        return "protocol-failed", match.group(1)
    return "input-or-runtime-failed", None


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(args):
    if len(args) not in (3,4) or (len(args)==4 and args[3]!="data-error"):
        raise SystemExit("usage: replay-actions.py EXE SOURCE_RUN NEW_OUTPUT [data-error]")
    executable,source,out=map(lambda value:Path(value).resolve(),args[:3])
    mutation=args[3] if len(args)==4 else None
    actions=[json.loads(line) for line in (source/"actions.jsonl").read_text().splitlines()]
    encoded=encode(actions)
    seed=json.loads((source/"canonical.jsonl").read_text().splitlines()[0])["seed"]
    if type(seed) is not int or not 0 < seed <= 0xffffffff:
        raise ValueError("source has invalid seed")
    configuration=source/"cchitest.ini"
    # Require a new directory and copy immutable inputs before execution.
    out.mkdir(parents=True,exist_ok=False)
    replay=out/"replay-actions.txt"; replay.write_text(encoded)
    fixture=out/"cchitest.ini"; fixture.write_bytes(configuration.read_bytes())
    evidence=dict(state="running",scope="complete intent diagnostic replay; no architectural acceptance",
        sourceRun=str(source),seed=seed,acceptedActions=len(actions),validationMutation=mutation,
        sourceActionsDigest=digest(source/"actions.jsonl"),replayDigest=digest(replay),
        configurationDigest=digest(fixture),binaryDigest=digest(executable))
    record=out/"replay-evidence.json"
    record.write_text(json.dumps(evidence,indent=2))
    command=[str(executable),str(seed),str(out/"traces"),"action-replay",str(replay),mutation or "-",str(fixture)]
    log=out/"run.log"
    try:
        with log.open("w") as stream:
            run=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,timeout=120)
        evidence.update(command=command,exitCode=run.returncode,log=str(log))
        if run.returncode:
            evidence["state"],signature=classify_failure(log.read_text())
            if signature: evidence["failureSignature"]=signature
        elif mutation:
            evidence["state"]="validation-not-detected"
        else:
            checker=Path(__file__).with_name("check-dut-trace.py")
            with (out/"checker.log").open("w") as stream:
                checked=subprocess.run([sys.executable,str(checker),str(out/"traces"/str(seed)),"action-replay"],
                    stdout=stream,stderr=subprocess.STDOUT,timeout=120)
            evidence["traceCheckExitCode"]=checked.returncode
            evidence["state"]="passed" if checked.returncode==0 else "checker-failed"
        evidence["inputsUnchanged"]=(digest(replay)==evidence["replayDigest"] and
            digest(fixture)==evidence["configurationDigest"] and digest(executable)==evidence["binaryDigest"])
        if not evidence["inputsUnchanged"]: evidence["state"]="inputs-changed"
    except subprocess.TimeoutExpired:
        evidence["state"]="timeout"
    record.write_text(json.dumps(evidence,indent=2)+"\n")
    print(json.dumps(evidence))
    return 0 if evidence["state"]=="passed" else 1


if __name__=="__main__":
    raise SystemExit(main(sys.argv[1:]))
