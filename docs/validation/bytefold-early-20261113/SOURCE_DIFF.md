# Source diff against the frozen deep1/PF0 candidate

The candidate is a source snapshot, not a patch against the formal kernel. The
comparison baseline is pinned in `BASELINE_SOURCE_MANIFEST.md`.

## DSP kernel

`kernel_impl.c` contains the only algorithm change. The new compile-time
`PRL_BYTEFOLD_EARLY=0|1` selector replaces the adaptive consumer-fold call at
the generic and specialized callbacks when mode 1 is selected. Mode 0 still
calls the frozen `snapshot/fused_adaptive_shared.h` helper. Mode 1 calls the
unchanged, pinned `fused_bytefold_early.h` helper with the same `s`, `cb`, raw
snapshot, scratch pointer, and existing signed-24 width bit. The producer,
corrected export, classification, synchronization, scratch, flush, and hash
code are copied unchanged. Compile-time checks force deep mode 1 and PF order
0; build flags keep G16 and four DSP workers fixed.

The remaining kernel-file change replaces the old PF-order runtime tag with
the separate family `0x42464500 | PRL_BYTEFOLD_EARLY`, returned by the actual
linked kernel tag function. No summary-word layout changes.

## Service and driver

`compound.idl` and the outer implementation rename the service to
`prl_compound_bytefold_batch`; the generated skeleton filename is checked as
`libprl_compound_bytefold_batch_skel.so`. `protocol.h` defines the independent
BFE tag family and strict mode parser. `driver.c` changes only the service
symbol, expected-mode input/output, runtime-tag validation, and marker names.
It retains the base's 32-nonce commitments, raw-TT/digest/mask CPU checks,
guards, warmup, and fail-fast behavior.

## Miner and app checker

The copied miner worker, nonce, hash, prep-pool, and app-check sources are
unchanged. The adapter's only behavior changes are the service binding and the
strict `PRL_EXPECT_BYTEFOLD_EARLY=0|1` check against every successful actual
word-63 tag, plus its corresponding log. Its default prep count remains four.
The host adapter and app-check mock now supply the mode-specific BFE tag; the
same app-check host gate runs both modes. `WITH_QNPU` remains enabled.

## Independent native comparison

`out/dsp/native_delta.json` is generated from each `kernel.o` disassembly and
ELF function `st_value`/`st_size`. The exact `pipeline_consume_35698` body
decodes to 293 instructions in mode 0 and 274 in mode 1; its normalized body
hashes differ. The report includes only observed `r29`-based memory references
and does not claim that other stack aliases were ruled out. It is static code
shape evidence, not a performance claim.
