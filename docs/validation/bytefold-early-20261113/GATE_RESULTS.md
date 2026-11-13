# Host/build gate receipt

Command run from this directory:

```sh
bash build.sh
```

The command reached its final success marker
`COMPOUND_BYTEFOLD_BUILD_OK modes=0,1 runtime_tag_family=0x42464500 deep=1 prefetch=0 G16=16 device_access=none`.
The build script uses `set -euo pipefail`, so each listed stage completed
successfully before that marker. The command's full stdout was returned in the
build session; a separate raw stdout log was not saved.

Observed gates:

- QAIC generated `prl_compound_bytefold_batch` and the expected skel basename.
- Both DSP variants compiled and linked with deep activation `Rt=6175`, weight
  `Rt=3968`, 48 paired deep sites, and six PF0 stage fetch sites.
- The native analyzer decoded the exact hot-callback ELF extent as 293 words
  for mode 0 and 274 for mode 1; the normalized bodies differ.
- The ARM 32-attempt correctness driver, `WITH_QNPU` miner, and app checker
  built in the experiment tree.
- Outer host checks passed; fail-fast source and fault-injection host gates
  passed; app-check host model passed both runtime modes; loader service/name,
  unique-CWD, and semicolon delimiter checks passed.
- Source and artifact hash manifests pass `sha256sum -c` in the experiment
  root and in `miner_20261113/`.

After the successful build, the final source-only freeze corrected the
nested-manifest write order to refresh the miner submanifest before the parent
manifest. No source logic or built artifact changed in that correction; both
manifest trees were regenerated and rechecked afterward.

Nonfatal compiler output included duplicate `byte` typedef warnings from the
generated QAIC headers. No device operation or pool test was run by this build.
