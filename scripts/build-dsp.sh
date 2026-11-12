#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${HEXAGON_SDK_ROOT:?set HEXAGON_SDK_ROOT to a Hexagon SDK 6.3 install}"
HK="${HEXKL_ROOT:?set HEXKL_ROOT to a hexkl_addon 1.0-beta.2 tree}"
OUT="${PRL_PAYLOAD_DIR:-$ROOT/payloads}"
BUILD="${PRL_BUILD_DIR:-$ROOT/build}"
UDMA="$SDK/addons/compute/libs/userdma/utils_lib"
TOOLS="$SDK/tools/HEXAGON_Tools/19.0.04/Tools/bin"
HEX="$TOOLS/hexagon-clang"
QAIC="$SDK/ipc/fastrpc/qaic/bin/qaic"

source "$ROOT/dsp/compound/build.conf"

HEXFLAGS=(
  -O3 -fPIC -std=gnu99 -mv79 -mhvx -mhmx -mhvx-length=128B -G0
  -fno-zero-initialized-in-bss -DUSE_OS_QURT
  -I"$SDK/incs/stddef" -I"$SDK/incs" -I"$SDK/rtos/qurt/computev79/include/qurt"
  -I"$SDK/rtos/qurt/computev79/include/posix" -I"$SDK/ipc/fastrpc/incs"
  -I"$SDK/ipc/fastrpc/remote/ship/inc" -I"$HK/include" -I"$UDMA/api" -I"$UDMA/inc"
)
LINKFLAGS=(-mv79 -mhvx -mhmx -mhvx-length=128B -G0 -fPIC
  -Wl,-Bsymbolic -Wl,--exclude-libs,ALL -Wl,--start-group
  "$HK/lib/6.3.0.0/hexagon_toolv88_v79/libhexkl_micro.a" -Wl,--end-group -lc -shared)
KERNEL_COMMON=(-Dprl_fused_fused_run=prl_compound_kernel_run
  -Dprl_fused_fused_run_hash=prl_compound_kernel_run_hash)

mkdir -p "$OUT" "$BUILD"/{gen_fused,gen,kernel_gen,dsp}

echo "== fused skeleton (selftest/fallback path) =="
"$QAIC" "$ROOT/dsp/fused/fused.idl" -o "$BUILD/gen_fused"
"$HEX" "${HEXFLAGS[@]}" \
  -DPRL_SPECIALIZE_HOT=1 -DPRL_SPECIALIZE_QUEUE=1 -DPRL_WAIT_FASTPATH=1 \
  -DPRL_ASYNC_STAGE=1 -DPRL_MM4_POINTER=0 -DPRL_ADAPTIVE_SHARED=0 \
  -DPRL_COMPACT_VTCM=2 -DPRL_SNAPSHOT_GROUP=16 -DPRL_GROUP_WORKERS=0 \
  -DPRL_HMX_CLOCK_POLICY=1 -DPRL_AUTO_WORKERS=1 -DPRL_DIAGNOSTIC_PROFILE=0 \
  -I"$BUILD/gen_fused" -I"$ROOT/dsp/fused" \
  "$BUILD/gen_fused/prl_fused_fused_skel.c" "$ROOT/dsp/fused/fused_impl.c" \
  "$UDMA/src/dma_utils.c" "${LINKFLAGS[@]}" -o "$OUT/libprl_fused_fused_skel.so"

echo "== compound skeleton ($PRL_SERVICE) =="
"$QAIC" "$ROOT/dsp/compound/compound.idl" -o "$BUILD/gen"
"$QAIC" "$ROOT/dsp/compound/snapshot/fused.idl" -o "$BUILD/kernel_gen"
INC=(-I"$BUILD" -I"$BUILD/gen" -I"$BUILD/kernel_gen" -I"$ROOT/dsp/compound/snapshot" -I"$ROOT/dsp/compound")
"$HEX" "${HEXFLAGS[@]}" "${INC[@]}" -c "$ROOT/dsp/compound/impl.c" -o "$BUILD/dsp/compound.o"
"$HEX" "${HEXFLAGS[@]}" "${INC[@]}" -c "$BUILD/gen/${PRL_SERVICE}_skel.c" -o "$BUILD/dsp/skel.o"
"$HEX" "${HEXFLAGS[@]}" "${INC[@]}" -fvisibility=hidden -c "$UDMA/src/dma_utils.c" -o "$BUILD/dsp/dma.o"

compile_kernel() { # $1 = object file, remaining args = extra defines
  local obj="$1"; shift
  "$HEX" "${HEXFLAGS[@]}" "${INC[@]}" -fvisibility=hidden \
    "${PRL_KERNEL_DEFINES[@]}" -DPRL_DIAGNOSTIC_PROFILE=0 "$@" "${KERNEL_COMMON[@]}" \
    -c "$ROOT/dsp/compound/kernel_impl.c" -o "$obj"
}
link_kernel() { # $1 = kernel object, $2 = output .so
  "$HEX" "${LINKFLAGS[@]}" "$BUILD/dsp/skel.o" "$BUILD/dsp/compound.o" "$1" \
    "$BUILD/dsp/dma.o" -o "$2"
}

if (( ${#PRL_KERNEL_MODES[@]} == 0 )); then
  compile_kernel "$BUILD/dsp/kernel.o"
  link_kernel "$BUILD/dsp/kernel.o" "$OUT/$PRL_SKEL_LIB"
else
  for mode in "${PRL_KERNEL_MODES[@]}"; do
    compile_kernel "$BUILD/dsp/kernel_deep$mode.o" -DPRL_MM4_DEEP="$mode"
    if [[ "$mode" == "${PRL_PACKAGE_MODE:?}" ]]; then
      link_kernel "$BUILD/dsp/kernel_deep$mode.o" "$OUT/$PRL_SKEL_LIB"
    fi
  done
fi

echo "== payload hashes =="
sha256sum "$OUT"/*.so
