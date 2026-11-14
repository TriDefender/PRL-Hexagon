#!/usr/bin/env bash
# Usage: scripts/build-apk.sh [debug|release]
#
# Auto-picks every supported SoC's payload folder into one APK. Each folder
# is produced by scripts/build-dsp.sh with the matching PRL_SOC:
#   SM8750/ -> Hexagon v79 (Snapdragon 8 Elite)
#   SM8650/ -> Hexagon v75 (Snapdragon 8 Gen 3)
# common/ holds the arch-independent miner and app-check (build-miner.sh).
# Skeletons are stored under SoC-suffixed jniLibs names; the app resolves
# this device's SoC at self-check and stages the matching pair under the
# canonical FastRPC names (DspArch.kt / MinerRunner.kt). Adding a future
# SoC = one folder built + one table entry here + one DspArch entry.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMON="${PRL_PAYLOAD_COMMON:-$ROOT/common}"
VARIANT="${1:-release}"
JLIBS="$ROOT/app/app/src/main/jniLibs/arm64-v8a"

# soc:dsp-e_flags pairs — the SoC folder order doubles as the DspArch order.
PAIRS=("SM8750:0x79" "SM8650:0x75")

source "$ROOT/dsp/compound/build.conf"
for pair in "${PAIRS[@]}"; do
  soc="${pair%%:*}"; flag="${pair#*:}"
  dir="$ROOT/$soc"
  # Fail loudly rather than skipping: an APK without this SoC's payload
  # would only surface as a self-check failure on that device.
  for f in libprl_fused_fused_skel.so "$PRL_SKEL_LIB"; do
    [[ -f "$dir/$f" ]] || {
      echo "missing $dir/$f — build it first: PRL_SOC=$soc bash scripts/build-dsp.sh" >&2
      exit 1
    }
  done
  python3 "$ROOT/scripts/check-dsp-elf.py" --arch "$flag" \
    "$dir/libprl_fused_fused_skel.so" "$dir/$PRL_SKEL_LIB"
done
for f in libprlminer.so app-check; do
  [[ -f "$COMMON/$f" ]] || { echo "missing $COMMON/$f (run build-miner.sh)" >&2; exit 1; }
done

rm -f "$JLIBS"/*.so
cp "$COMMON/libprlminer.so" "$JLIBS/"
cp "$COMMON/app-check" "$JLIBS/libprlappcheck.so"
for pair in "${PAIRS[@]}"; do
  soc="${pair%%:*}"
  cp "$ROOT/$soc/libprl_fused_fused_skel.so" "$JLIBS/libprl_fused_fused_skel_$soc.so"
  cp "$ROOT/$soc/$PRL_SKEL_LIB" "$JLIBS/${PRL_SKEL_LIB%.so}_$soc.so"
done

cd "$ROOT/app"
if [[ -x ./gradlew ]]; then GRADLE=./gradlew; else GRADLE=gradle; fi
"$GRADLE" --no-daemon "assemble${VARIANT^}"
APK="$(find "$ROOT/app/app/build/outputs/apk" -name "*.apk" -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2)"
mkdir -p "$ROOT/out"
cp "$APK" "$ROOT/out/"
echo "== APK =="
sha256sum "$ROOT/out/$(basename "$APK")"
