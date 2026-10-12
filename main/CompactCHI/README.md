# Compact CHI tester for AnserL2

This frontend runs the standalone AnserL2 DUT through the existing Sequencer,
Requester, Home and MemoryAgent. TileLink targets keep their original behavior.

## Current work

The top-level prefetch generator is the physical CoupledL2 PBOP teacher: the
37-candidate short table, `badScore=1`, s3 demand training and local Stash terminal
feedback. VBOP is not implemented or instantiated; its virtual/TLB boundary is
explicitly deferred. Exclusive, Atomic and WriteUnique remain Illegal.

`prefetch.jsonl` records accepted training, issued/dropped requests, terminal
feedback and training drops on real cycles. `check-prefetch.py` independently
replays the delay/RR/score teacher from accepted demand intent, checks same-page
predicted addresses, source, needT and TraceTag, and joins accepted Stash feedback.
Every observation is compared with top-level VCD. Issued observations come from
actual Slice acceptance; issue and drop are mutually exclusive. DUT offsets,
scores, RR contents, returned bytes or replacement choices never generate
expected predictions.

The normal checker uses observed accepted training as the teacher-model input and
matches each sample to independent demand intent. This proves learning/output and
feedback behavior; it does not prove all eligible demands were trained. The
optional `--prefetch-training-coverage` check adds an independent coverage proof
only for reset-empty, unique first-access ReadOnce traffic with PBOP-only
allocation, no maintenance/errors and no s3 snapshot drops. Other qualification
completeness remains open. A merged demand snapshot is generated at its first s3
arrival even when refill retry cancels installation, so retry cannot swallow the
one-shot dispatch token.

The existing independent architectural replay checks Requester/Home memory and
installed L2 versions. Directory/DataStorage scans require drained resources and
compare actual VCD storage against independently derived values. Residency proof
covers under-capacity groups and the fixed victim vector; unknown overflow and
ambiguous same-line ordering stay explicitly excluded.

Forward endpoint evidence uses DUT1/forward12/Home8 with CompAck outside the DUT.
The separate host multi-forward fixture proves at most two concurrent stable-line
obligations. This is not arbitrary external network topology equivalence.

## Validation

From the parent repository, run `make tester-foundation`, then finite native
profiles: `deterministic-test`, `random-read-test`, `lower-snoop-test`, `victim-test`,
`node-actions-test` and `configured-node-test`. All use the standard tester INI
schema; caller-owned `tltest.ini` is never overwritten.

Canonical handshakes, stall stability, Retry/issuer-type credit consumption,
completion role/DBID routing, generated transitions, resource drain and storage
remain required alongside prefetch evidence. A sustained transient ReadOnce stream
is used to demonstrate actual PBOP learning, issuance and terminal completion.
Host matrix inputs are reported separately from native DUT coverage.

`make global-test` remains an explicit rejection guard returning 2. No finite
profile publishes full architectural global acceptance. VBOP, full errors,
nested same-line overlap, auxiliary overflow replacement and arbitrary topology
remain open. Current acceptance records, source/tester/runner/configuration
fingerprints, logs and waveforms live under the parent's ignored `build/`.
