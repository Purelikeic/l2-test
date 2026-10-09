#!/usr/bin/env python3
"""Replay and delta-reduce a serialized read stimulus while preserving failure identity.

This tool does not reinterpret illegal multi-owner traffic as a DUT failure.
Candidates count only when the original protocol failure message recurs. The
optional mutation argument verifies the integrated board/reducer itself and is
never used by ordinary DUT acceptance.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

if len(sys.argv) not in (5, 6):
    raise SystemExit("usage: minimize-read-trace.py EXE SEED STIMULUS NEW_BUILD_DIR [data-error]")
exe = str(Path(sys.argv[1]).resolve())
seed = sys.argv[2]
lines = [line for line in Path(sys.argv[3]).read_text().splitlines() if line.strip()]
root = Path(sys.argv[4]).resolve()
root.mkdir(parents=True, exist_ok=False)
mutation = sys.argv[5] if len(sys.argv) == 6 else None
if mutation not in (None, "data-error"):
    raise SystemExit("unsupported validation mutation")
attempts = 0

def evaluate(candidate):
    global attempts
    trial = root / f"trial-{attempts:04d}"
    trial.mkdir()
    stimulus = trial / "stimulus-input.txt"
    stimulus.write_text("\n".join(candidate) + "\n")
    command = [exe, seed, str(trial), "random-read", str(stimulus)]
    if mutation: command.append(mutation)
    try:
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=30)
        output = re.sub(r"\x1b\[[0-9;]*m", "", run.stdout)
        (trial / "run.log").write_text(output)
        match = re.search(r"Protocol cycle failed: ([^\n]+)", output)
        signature = match.group(1) if run.returncode and match else None
    except subprocess.TimeoutExpired:
        signature = None
    attempts += 1
    return signature

signature = evaluate(lines)
if signature is None:
    raise SystemExit("No reproducible protocol failure; passing/timeout/illegal-startup trials are not reduced.")
original_count = len(lines)
chunks = 2
while len(lines) >= 2:
    size = (len(lines) + chunks - 1) // chunks
    changed = False
    for start in range(0, len(lines), size):
        candidate = lines[:start] + lines[start + size:]
        if candidate and evaluate(candidate) == signature:
            lines = candidate
            chunks = max(2, chunks - 1)
            changed = True
            break
    if not changed:
        if chunks >= len(lines): break
        chunks = min(len(lines), chunks * 2)
(root / "minimized-stimulus.txt").write_text("\n".join(lines) + "\n")
if evaluate(lines) != signature:
    raise SystemExit("Reduced trace did not reproduce the same failure on confirmation.")
evidence = {"seed": int(seed), "originalRequests": original_count, "minimizedRequests": len(lines),
            "failure": signature, "attempts": attempts, "validationMutation": mutation,
            "scope": "serialized read-profile reduction; one-minimal by tested deletion chunks"}
(root / "minimization.json").write_text(json.dumps(evidence, indent=2))
print(json.dumps(evidence))
