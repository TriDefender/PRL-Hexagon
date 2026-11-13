# v1.0.0 release compound library failed to load (issue #1)

## Symptom

The published v1.0.0 APK passed all five startup checks, but the first
compound RPC stopped the miner with `rc=44 (0x2c)` and zero TMAC/s. The
report came from a Xiaomi 15 (SM8750, Android 16), the same SoC family as
the development device.

## Root cause

The compound skeleton packaged in the release APK was not the payload that
had been tested locally:

| artifact | SHA-256 | undefined `hexkl_*` imports |
|---|---|---|
| release `libprl_compound_deep4_batch_skel.so` | `adf089517be3213ea0af71d3ab384c5f46f9386e160c06d5198d9f974c2300ae` | 8 |
| release `libprl_fused_fused_skel.so` | `4c5f39c2baecc3dbfe14903c44bdf84f22e916fa60efe8240f30939ffbcbf1c6` | 0 |

At tag `v1.0.0`, `scripts/build-dsp.sh` put `libhexkl_micro.a` in
`LINKFLAGS`, before the object files that reference it. The Hexagon linker
therefore did not extract the required archive members, but the `-shared`
link still succeeded. The fused skeleton does not use those symbols, so the
five-item self-check passed; the missing code only failed when the app tried
to open the compound handle, producing the loader error and `rc=44`.

This is a build-order regression in the release path, not a device-specific
file or a dependency on the local test machine.

## Fix

`scripts/build-dsp.sh` now links static archives after the objects. A new
`scripts/check-dsp-elf.py` gate rejects unresolved `hexkl_*` imports and
invalid `DT_INIT`/`DT_FINI` mappings in both DSP construction and APK
packaging, including cached payloads. The rebuilt bytefold skeleton is
`e6447377cfa1a22b25b7589366df93900fba3379b54cdb32d556925415caf0a1` and passes
the gate.

The app also runs the compound kernel itself during startup self-check
(`payloads/app-check`), so a payload that cannot be loaded is reported as
"unsupported" before mining starts.

## App-domain validation

- Device: OnePlus 13 (PLQ110), SM8750, Android 16
- APK: debug build from `fix/compound-selfcheck`,
  `9782131afc6a22254349fc047a661b5a9c4a694e2c4b61c904739a51f4a816eb`
- Packaged compound skeleton: `e6447377...` (153192 bytes)
- Result: all six checks pass, including
  `compound -> PASS 复合内核校验通过，这台手机可以挖矿`
- FastRPC log:
  `remote_handle_open: Successfully opened handle ... for prl_compound_bytefold_batch on domain 3 (load time 3539 us)`
- No `RX VA ... outside ELF segment` and no `rc=44` occurred.

The check runs the same 2-job / 4-batch compound path as mining. No pool
share was submitted during this validation, so share acceptance remains
outside its scope.

The published v1.0.0 assets were not modified; a new release built from the
fixed sources is required.
