#!/usr/bin/env bash
# Usage: scripts/build-apk.sh [debug|release]
#
# Packages every supported DSP arch into one APK. Each payload set comes from
# its own build-dsp.sh run (PRL_DSP_ARCH selects the target):
#   payloads/     -> Hexagon v79 (Snapdragon 8 Elite, SM8750)  [default dir]
#   payloads-v75/ -> Hexagon v75 (Snapdragon 8 Gen 3, SM8650)
# The skeletons are stored under arch-suffixed jniLibs names; the app resolves
# the device arch at self-check and stages the matching pair under the
# canonical FastRPC names (see DspArch.kt / MinerRunner.kt).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V79="${PRL_PAYLOAD_V79:-${PRL_PAYLOAD_DIR:-$ROOT/payloads}}"
V75="${PRL_PAYLOAD_V75:-$ROOT/payloads-v75}"
VARIANT="${1:-release}"
JLIBS="$ROOT/app/app/src/main/jniLibs/arm64-v8a"

# arch:dir pairs, in the order DspArch.TARGETS lists them.
source "$ROOT/dsp/compound/build.conf"
PAIRS=("v79:0x79:$V79" "v75:0x75:$V75")

for pair in "${PAIRS[@]}"; do
  suffix="${pair%%:*}"; rest="${pair#*:}"
  flag="${rest%%:*}"; dir="${rest#*:}"
  python3 "$ROOT/scripts/check-dsp-elf.py" --arch "$flag" \
    "$dir/libprl_fused_fused_skel.so" "$dir/$PRL_SKEL_LIB"
  for f in libprl_fused_fused_skel.so "$PRL_SKEL_LIB"; do
    [[ -f "$dir/$f" ]] || { echo "missing $dir/$f (run build-dsp.sh for this arch)" >&2; exit 1; }
  done
done
# The miner and app-check are arch-independent; ship one copy each.
for f in libprlminer.so app-check; do
  [[ -f "$V79/$f" ]] || { echo "missing $V79/$f (run build-miner.sh)" >&2; exit 1; }
done

rm -f "$JLIBS"/*.so
# miner + app-check are arch-independent: take them from the v79 set.
cp "$V79/libprlminer.so" "$JLIBS/"
cp "$V79/app-check" "$JLIBS/libprlappcheck.so"
# Skeletons carry the arch suffix the app selects at runtime.
cp "$V79/libprl_fused_fused_skel.so" "$JLIBS/libprl_fused_fused_skel_v79.so"
cp "$V79/$PRL_SKEL_LIB" "$JLIBS/${PRL_SKEL_LIB%.so}_v79.so"
cp "$V75/libprl_fused_fused_skel.so" "$JLIBS/libprl_fused_fused_skel_v75.so"
cp "$V75/$PRL_SKEL_LIB" "$JLIBS/${PRL_SKEL_LIB%.so}_v75.so"

cd "$ROOT/app"
if [[ -x ./gradlew ]]; then GRADLE=./gradlew; else GRADLE=gradle; fi
"$GRADLE" --no-daemon "assemble${VARIANT^}"
APK="$(find "$ROOT/app/app/build/outputs/apk" -name "*.apk" -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2)"
mkdir -p "$ROOT/out"
cp "$APK" "$ROOT/out/"
echo "== APK =="
sha256sum "$ROOT/out/$(basename "$APK")"
