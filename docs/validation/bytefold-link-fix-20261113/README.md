# Fix missing static HexKL code in the published DSP library

The release build moved the HexKL archive into `LINKFLAGS`, before the
compound object files. The Hexagon linker did not extract the required
archive members. Its shared-library link still succeeded, leaving eight
`hexkl_micro_*` imports unresolved in the result. The frozen device-tested
candidate links its objects before the archive and does not have this defect.

The rebuilt kernel objects were identical to the tested objects, but this was
insufficient to establish correctness of the linked release payload. Previous
validation did not cover the rebuilt APK in the App domain. The user's
compound self-check exposed a dynamic loading failure before any compute
result was available; failing payloads must continue to be ignored.

`scripts/build-dsp.sh` now places the HexKL archive and libc after the object
files. `scripts/check-dsp-elf.py` checks the actual ELF dynamic symbol table and
rejects unresolved `hexkl_*` imports. It also checks executable file-backed
segments and DT_INIT/DT_FINI addresses. QuRT/HAP/libc imports remain allowed
because firmware supplies them. Both DSP construction and APK packaging run
this check, including when APK packaging reuses cached payloads.

Validation completed:

- The guard rejects the actual previously published bad local payload with
  all eight unresolved HexKL names; this is a regression check against the
  observed build failure, rather than a simulated HMX correctness test.
- DSP rebuild succeeds and both output skeletons pass the guard.
- Both mode0/mode1 kernel object files remain byte-identical to the frozen
  device-tested objects; the mining algorithm has not changed.
- Shell and Python syntax checks pass.

`ELF_COMPARISON.json`, `KERNEL_OBJECTS.json`, and `build-dsp.txt` preserve these
results. Static archive position is described in the [GNU linker
manual](https://sourceware.org/binutils/docs-2.29/ld/Options.html).

App-domain validation is a separate gate. An initial load-only probe under
the existing App UID returned a permission error (114), not the reported
loader error (44); the application was subsequently found uninstalled.
That interrupted environment does not establish a successful App-domain
retest. The probe made no compute calls, temporary files were removed, and
the existing native payload and formal skeleton hashes were unchanged.
The App-domain outcome must be recorded after a stable App installation is
available; no new APK success claim is made from this static build check.
