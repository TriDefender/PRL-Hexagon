#!/usr/bin/env bash
# Usage: scripts/build-apk.sh [debug|release]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${PRL_PAYLOAD_DIR:-$ROOT/payloads}"
VARIANT="${1:-release}"
JLIBS="$ROOT/app/app/src/main/jniLibs/arm64-v8a"

source "$ROOT/dsp/compound/build.conf"
for f in libprlminer.so libprl_fused_fused_skel.so "$PRL_SKEL_LIB" app-check; do
  [[ -f "$OUT/$f" ]] || { echo "missing payloads/$f (run build-dsp.sh and build-miner.sh)" >&2; exit 1; }
done

rm -f "$JLIBS"/*.so
cp "$OUT/libprlminer.so" "$OUT/libprl_fused_fused_skel.so" "$OUT/$PRL_SKEL_LIB" "$JLIBS/"
cp "$OUT/app-check" "$JLIBS/libprlappcheck.so"

cd "$ROOT/app"
if [[ -x ./gradlew ]]; then GRADLE=./gradlew; else GRADLE=gradle; fi
"$GRADLE" --no-daemon "assemble${VARIANT^}"
APK="$(find "$ROOT/app/app/build/outputs/apk" -name "*.apk" -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2)"
mkdir -p "$ROOT/out"
cp "$APK" "$ROOT/out/"
echo "== APK =="
sha256sum "$ROOT/out/$(basename "$APK")"
