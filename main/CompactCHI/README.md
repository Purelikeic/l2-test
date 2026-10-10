# Compact CHI testing

## Current terminal architectural scoreboard boundary

Successful native finish now also requires exact terminal Requester/Home storage
agreement with ArchitecturalBoard. The observed requester map is copied from
Requester storage, including every identity, line, permission and 64-byte value.
Every initialized cache line is read from the actual MemoryAgent, and the whole
accessed Home device map is copied separately. Expected versions remain driven
by initialized data, independently checked actions and validated memory commits.
Latest cache data and committed memory are separate: clean/dirty requester data
need not equal Home bytes. ReadOnce and MMIO create no persistent owner.

architectural-state.json records both terminal maps at the successful cycle.
check-architectural-state.py independently replays action intent and canonical
handshakes, derives requester residency/permissions/latest data, copyback and
lower-Snoop Home versions, CMO discard and MMIO masked bytes, and checks both
maps. It rejects missing/extra entries and mutated identities/permissions/data.
The VCD/channel and continuous resource-drain checks remain required alongside
this check. The terminal map is not a source of expected replay values.

This is a bounded endpoint architectural check. Internal L2 Directory/DataStorage
residency, complete external forward-node state, all nesting/error/concurrency
and full ARI/unified equivalence remain open. Second persistent owners remain
Deferred; WriteUnique/Exclusive/Atomic remain Illegal; global-test remains guarded.
PBOP/VBOP and functional protocols are unchanged.

## Historical DUT resource observation and terminal drain

ResourceStatus is a read-only RTL summary of occupied ownership layers, exported
by Slice/top. It includes live MSHR and upstream response contexts, all WBF and
Snoop data reservations, request-buffer entries, task/Snoop/TXREQ queues, valid
pipeline/buffer-response slots, serializer activity, held/control/prefetch/MMIO/
Flush activity, RetryAck owners and early P-Credits. Counts are not summed into
a distinct transaction count: transfer copies can occupy several layers.

No summary feeds functional arbitration, ready, FSM or capacity control. Counts
use current validity, not same-cycle free/reuse lookahead. Retired data bits in
Refill/WBB arrays and architectural resident cache lines are not live resources;
their active ownership is represented by contexts. ready marks completed Slice
initialization. This observation is not Directory residency or architectural
permission/data correctness, which remains with independent boards.

Native ResourceMonitor records resources.jsonl every cycle with actual simulation
time and requires observed activity plus terminal zero counts. Successful finish
requires all resource fields zero/ready alongside existing independent protocol
obligations and twenty settle cycles. A quiet wire can no longer hide a response
context, queued data or early credit. The resource checker verifies continuous
sampling, terminal window and thirteen leak/readiness/activity/gap mutations;
the original trace checker compares every resource sample with top-level VCD
ports, including cycles without a channel event.

Focused RTL checks cover MSHR-to-serializer transfer, a quiet response context
waiting for upstream CompAck, Snoop reservations absent from WBF-only info,
current occupancy versus release lookahead, and retained early credits. The host
resource monitor rejects live slots in each of ten observation categories.
This closes the observed resource drain boundary, not full architecture/global
acceptance or every latent resource implementation. PBOP/VBOP, full scoreboard,
all concurrency/error/nested scenarios and final architectural residency remain
pending; global-test stays guarded.

## Historical configured Sequencer nodes — before resource drain observation

Both protocols use the original TLLocalConfig/LoadTestConfig and tltest.ini schema.
The native binding calls TLSequencer::Initialize(config, adapter). Sequencer
constructs a configured per-core layout and asks the adapter to replace node
implementations: TL-C -> coherent Type 1, TL-UL -> ReadOnce transient, one MMIO
node per core -> ReadNoSnp/WriteNoSnp. Shared Compact wire IDs assign MMIO a
non-overlapping region after cache-node IDs. AnserL2.cpp does not set node counts.

NodeFuzzer is the only production Compact stimulus scheduler. It emits protocol-
neutral TestActions; NodeAgent/Requester encode them. CFuzzer has no Compact
branch and the former native ReadFuzzer is removed. Seed, startup, original ARI
ranges/permutation/interval/target and stream parameters come from the same INI.
Finite deterministic/random/lower-snoop/victim/node-actions are scheduler modes;
INI mode.X can select deterministic_read, random_read, lower_snoop, victim or
node_actions. Configured mode also supports original unified U/R/B policy with
explicit full-overwrite intent and agent admission; conflicting persistent owners
remain Deferred, never silently approximated. Rejections are separately recorded.

Type 1 actions include ReadShared/ReadUnique, explicit full-line MakeUnique
(overwrite after CompAck), clean Evict, dirty WriteBack, and three CMO classes.
Combined WriteBack completion retains its data obligation. Transient/MMIO nodes
do not acquire persistent ownership. A queued or granted second persistent owner
is Deferred; ArchitecturalBoard independently checks it. Exclusive, Atomic and
WriteUnique always return Illegal. Snoop targeting transient/MMIO is rejected.

CMO and MMIO use existing Requester/ArchitecturalBoard/EbHome transport with
independent intent/data snapshots. Device byte memory is separate from expected
MMIO data; dirty cache copyback still commits through original MemoryAgent.
DCT forward acknowledgment is now scheduled on a later cycle by the endpoint
service. Quietness is still not proof of complete internal occupancy.

`make tester-foundation` includes configured-node and protocol-neutral scheduler
host checks. `make node-actions-test` covers permission overwrite, release/CMO
and MMIO; `make configured-node-test` uses INI ARI/stream/unified modes. All finite
profiles use `TLTEST_INI`, defaulting to this directory's config/tltest.ini fixture.
Set that variable to an existing standard INI to vary topology without changing
protocol adapter code. The caller's root tltest.ini is never overwritten.
Topology, selected scenario, accepted actions and rejections are saved alongside
canonical/VCD traces. The checker derives role IDs from the configured topology
and verifies node counts against the exact input INI snapshot.

A native typed-node scenario exposed Snoop-copy metadata being treated as an
invalidating ReleaseData in the DUT. The scoped RTL repair preserves the received
SC/UC client snapshot during Snoop copying, while real invalidating release still
clears it. The independent CMO checker retains the client until actual invalidation.

These are bounded node/profile checks. Complete ARI/unified architectural state,
all concurrency/error/nested variants, internal occupancy and prefetch integration
remain unfinished. Global-test stays guarded; no full global pass is published.

## Historical binding and scoped victim adapter — before configured nodes

AnserL2.cpp binds the only DUT, AnserL2, through original System LoadTestConfig,
TLSequencer InitializeProtocol/Tick/Tock/Finalize, Fuzzer, boards and MemoryAgent.
The original framework is connected; the historical frontend claims below are
archived and do not describe current architectural acceptance.

From the AnserL2 parent, `make tester-foundation` runs ten host-only gates,
including the independent victim reference model and eight rejected mutations.
It does not run a second DUT. Existing deterministic-read, random-read and
lower-snoop profiles remain finite scenario evidence.

`make victim-test SEEDS="42 7 12345 17 9999"` elaborates the default four-slice,
128-set, four-way DUT and runs fixed independent replacement vectors on original
Home memory. The profile issues ten serialized reads: fill A..H at a same-set
stride, modify the unique requester copy of A, reload evicted A from memory, then
hit resident F. It checks five primary victims A..E, dirty-client recall,
WriteBackFull, both clean WriteEvictOrEvict completion branches, exact copyback
data and real MemoryAgent commitment. Root BOP auxiliary traffic occupies the
adjacent slice and retains a separate clean-traffic boundary.

VictimProfile.hpp checks planned request/victim order and classification;
EbHome supplies existing independent data/DBID/CompAck and memory checks.
CopyBackWrData is routed to EbHome, while Snoop/DCT data retain LowerSnoopBoard.
check-victim-events.py independently checks fixed trace vectors, dirty data,
branch obligations and eight invalid trace observations. check-dut-trace.py
retains the existing exact VCD/offer-stability check for all directions.

Records are in ignored parent build/victim-acceptance.json and
build/compactchi-victim/<seed>/. Source, tester, foundation-gate and runner
fingerprints must remain unchanged during acceptance. The runner fingerprint
includes its own source, source/foundation helper scripts and parent Makefile;
the tester fingerprint includes native source, headers and checkers.

This is limited replacement evidence: no CMO/MMIO/ARI/full concurrent topology,
complete internal resource/residency model, PSEL/leader sweep or new PBOP/VBOP
algorithm acceptance. Twenty quiet cycles are only a settle condition. Current
CMO component/Slice checks remain with AnserL2 ChiselTests. Full architectural
scoreboard and independent forward-node/internal occupancy exits remain pending.
Global-test remains an explicit rejection guard; these profiles never publish
full global acceptance. Compact WriteUnique, Exclusive, Atomic, ECC and early
Hint behavior are outside this adapter.

## Historical foundations/frontend notes — superseded by current binding

### Verified tester foundations — historical checkpoint

The final target must use AnserL2 as its only DUT and the original tester
System/Sequencer/MemoryAgent/boards/fuzzer lifecycle. The legacy frontend below
is still present; it is not the finished framework/architectural acceptance
method. Prefetch/replacement development must wait for actual DUT binding and
complete independent scoreboard obligations.

From the AnserL2 parent, `make tester-foundation` runs seven serialized host
component checks and records exact RTL, tester and gate digests with unchanged
source checks in ignored `build/tester-foundation-acceptance.json`:

- Original MemoryAgent portability, real seed use, latency, OOO, masked writes,
  backpressure and 64-bit cycle handling.
- CHI HomeMemory line adapter backed by the original memory queues, including
  reserved completion capacity and borrowed Sequencer resource mode.
- Shared original System defaults/callback/INI settings via `LoadTestConfig`.
- Independent ArchitecturalBoard using `ScoreBoard`, `GlobalBoard` and
  `UncachedBoard`: legal reads, dirty node ownership, complete beats, DBID,
  CompAck, four Compact reverse-Snoop opcodes and 23 invalid-observation checks.
- Original `TLSequencer::InitializeProtocol`/Tick/Tock/Finalize lifecycle:
  protocol adapter hooks, original Fuzzer base and borrowed boards/memory,
  finish/failure handling, plus the default TileLink idle lifecycle.
- Canonical JSONL/text sink for all 13 Compact/E.b port directions; cycle,
  optional simulation time, TraceTag, handshake, ID/address/data/state fields.
  Unknown fields are null. Independent JSON parsing verifies the host fixture.
- Read-profile Requester adapter: node/TxnID versus DBID association, CompAck
  resource lifetime, dirty Snoop data and independent offered queues under stalls.

The shared-cycle host fixture combines requester, HomeMemory, architectural
board and canonical records. Its real host cycles and data are independently
parsed from `build/compactchi-foundation/native-host-trace.jsonl`; it does not
contain a DUT or a waveform. Simulation time is explicitly null there.

The initial board does not model E.b Snoop/DCT, Retry/P-Credit, CMO, writeback,
MMIO, replacement/internal L2 state or prefetch algorithms. Requester currently
supports normal full-line read requests, CompAck and Compact reverse Snoops;
other transaction classes still require adapters. These host tests cannot be
used as full DUT alignment evidence. No reference RTL is built or run, and no
new database/DPI dependencies are used by this path.

## Legacy standalone frontend — historical control traffic

This frontend adds a Compact CHI requester and downstream CHI home environment
for AnserL2. It drives the generated `L2Cache` ports directly. The original
TileLink agents, port generator, CMake targets and scenarios retain their
existing behavior; they cannot treat Compact CHI DBID/CompAck/Snoop ownership as
TileLink source/sink bookkeeping.

From the AnserL2 repository, run `make unit` first. `make global-test` checks the
exact source digest in the passing unit gate, generates SV, and calls this
submodule's build target. All generated files are placed in the parent's
ignored `build/` directory. With already generated SV, the frontend can also be
built explicitly:

```sh
make compactchi-test DUT_SV=/path/to/AnserL2/build/L2Cache.sv THREADS_BUILD=4
```

`COMPACTCHI_OBJ_DIR` selects an ignored simulator directory and
`COMPACTCHI_SEEDS` selects nonzero 32-bit seeds. No DUT, Rocket-Chip, SQLite,
TileLink port-generation or nested submodule build is required for this target.

The environment exercises four Slices with mixed ReadOnce/ReadShared/ReadUnique,
alias changes, clean and dirty victims, both E.b WriteEvictOrEvict completion
branches, early P-Credits, either beat order, CompData and separated completion
with distinct RSP SrcID/DAT HomeNID, and independent backpressure including
50-cycle stalls. Shared/Once/Unique/CleanInvalid/MakeInvalid and forwarded Shared/
Unique Snoops exercise both Home and DCT responses. Demand traffic is followed
by a MakeUnique miss and full-line requester overwrite, a ReadOnce that receives
two dirty SnpRespData beats, re-acquisition of Unique, a modified WriteBackFull,
a same-address downstream Snoop, and a cancelled/restarted internal Flush.
Success requires complete demand responses, no duplicate data beat, exact
256-bit data matches, stable output flits during stalls, exact downstream
CompAck identities, complete writeback/Snoop/DCT tracking, and `flushDone`.

The bounded frontend is intended for the frozen AnserL2 topology: one included
L1D, four Slices, 64-byte lines, 256-bit beats. It is not a conversion of the
existing multi-core TileLink fuzzer and does not claim support for Atomic,
Exclusive or new ECC transactions. Non-ECC errors and additional protocol
variants are covered by AnserL2's focused ChiselTests.
