# Compact CHI standalone frontend

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
