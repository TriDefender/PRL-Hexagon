#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${HEXAGON_SDK_ROOT:?set HEXAGON_SDK_ROOT to a Hexagon SDK 6.3 install}"
NDK="${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT to an Android NDK r27c install}"
# The ARM miner and app-check are DSP-arch independent; per-SoC DSP payloads
# live in their own folders (scripts/build-dsp.sh). The APK takes the miner
# from here and the skeletons from the SoC folders.
OUT="${PRL_PAYLOAD_DIR:-$ROOT/common}"
BUILD="${PRL_BUILD_DIR:-$ROOT/build}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"

source "$ROOT/dsp/compound/build.conf"

mkdir -p "$OUT" "$BUILD"/{gen_fused,arm_gen}
"$SDK/ipc/fastrpc/qaic/bin/qaic" "$ROOT/dsp/fused/fused.idl" -o "$BUILD/gen_fused"
"$SDK/ipc/fastrpc/qaic/bin/qaic" "$ROOT/dsp/compound/compound.idl" -o "$BUILD/arm_gen"

CFLAGS=(
  -O3 -std=gnu11 -DPRL_BATCH_ATTEMPTS=32 -DBLAKE3_USE_NEON=1
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
  -DWITH_QNPU "${PRL_MINER_DEFINES[@]}"
  -I"$ROOT/miner" -I"$ROOT/miner/blake3" -I"$BUILD/gen_fused" -I"$BUILD/arm_gen"
  -I"$ROOT/dsp/fused" -I"$ROOT/dsp/compound"
  -I"$SDK/incs" -I"$SDK/incs/stddef"
  -I"$SDK/ipc/fastrpc/incs" -I"$SDK/ipc/fastrpc/remote/ship/inc"
  -I"$SDK/ipc/fastrpc/rpcmem/inc"
)
B3="$ROOT/miner/blake3"
M="$ROOT/miner"
LIBS=(
  -L"$SDK/ipc/fastrpc/remote/ship/android_aarch64" -lcdsprpc
  "$SDK/ipc/fastrpc/rpcmem/prebuilt/android_aarch64/rpcmem.a"
  -lm -ldl -pthread
)

"$CC" "${CFLAGS[@]}" \
  "$M/main.c" "$M/pearl.c" "$M/mine.c" "$M/proof.c" "$M/stratum.c" "$M/qnpu_fused.c" \
  "$BUILD/gen_fused/prl_fused_fused_stub.c" "$BUILD/arm_gen/${PRL_SERVICE}_stub.c" \
  "$B3/blake3.c" "$B3/blake3_dispatch.c" "$B3/blake3_portable.c" "$B3/blake3_neon.c" \
  "${LIBS[@]}" -o "$OUT/libprlminer.so"

"$CC" -O3 -std=gnu11 -DPRL_BATCH_ATTEMPTS=32 -DBLAKE3_USE_NEON=1 \
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
  "${PRL_MINER_DEFINES[@]}" \
  -I"$ROOT/miner" -I"$ROOT/miner/blake3" -I"$BUILD/gen_fused" -I"$BUILD/arm_gen" \
  -I"$ROOT/dsp/fused" -I"$ROOT/dsp/compound" \
  -I"$SDK/incs" -I"$SDK/incs/stddef" \
  -I"$SDK/ipc/fastrpc/incs" -I"$SDK/ipc/fastrpc/remote/ship/inc" \
  -I"$SDK/ipc/fastrpc/rpcmem/inc" \
  "$M/app_check.c" "$M/pearl.c" "$M/mine.c" "$M/proof.c" "$M/qnpu_fused.c" \
  "$BUILD/gen_fused/prl_fused_fused_stub.c" "$BUILD/arm_gen/${PRL_SERVICE}_stub.c" \
  "$B3/blake3.c" "$B3/blake3_dispatch.c" "$B3/blake3_portable.c" "$B3/blake3_neon.c" \
  "${LIBS[@]}" -o "$OUT/app-check"

# Independent full-size commitment/raw-TT/all-digest/all-mask CPU comparison.
"$CC" "${CFLAGS[@]}" -I"$ROOT/dsp/compound" \
  "$ROOT/dsp/compound/driver.c" "$BUILD/arm_gen/${PRL_SERVICE}_stub.c" \
  "$M/pearl.c" "$B3/blake3.c" "$B3/blake3_dispatch.c" \
  "$B3/blake3_portable.c" "$B3/blake3_neon.c" \
  "${LIBS[@]}" -o "$OUT/bytefold-batch32-driver"

sha256sum "$OUT/libprlminer.so" "$OUT/app-check" "$OUT/bytefold-batch32-driver"
