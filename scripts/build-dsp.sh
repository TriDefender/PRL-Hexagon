#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${HEXAGON_SDK_ROOT:?set HEXAGON_SDK_ROOT to a Hexagon SDK 6.3 install}"
HK="${HEXKL_ROOT:?set HEXKL_ROOT to a hexkl_addon 1.0-beta.2 tree}"
OUT="${PRL_PAYLOAD_DIR:-$ROOT/payloads}"
BUILD="${PRL_BUILD_DIR:-$ROOT/build}"
# Target compute DSP architecture: v79 (Snapdragon 8 Elite, SM8750) or
# v75 (Snapdragon 8 Gen 3, SM8650). 8 Gen 3's cDSP is Hexagon v75, with a
# v73 backward-compatibility target; the payload ELF must match the device.
ARCH="${PRL_DSP_ARCH:-v79}"
UDMA="$SDK/addons/compute/libs/userdma/utils_lib"
TOOLS="$SDK/tools/HEXAGON_Tools/19.0.04/Tools/bin"
HEX="$TOOLS/hexagon-clang"
QAIC="$SDK/ipc/fastrpc/qaic/bin/qaic"

source "$ROOT/dsp/compound/build.conf"

if [[ "$ARCH" == "v75" ]]; then
  # The v75 cDSP (Snapdragon 8 Gen 3) exposes four HVX contexts. The stock
  # hot path is compiled for four fold workers plus a producer and an async
  # stage thread — six concurrent qurt_hvx_lock(128B) users — which cannot
  # lock on v75 and aborts the run. Every orchestration layout must fit
  # producer + optional stage thread + fold workers inside four contexts.
  #   PRL_V75_WORKERS       fold pool size (default 2, with stage thread)
  #   PRL_V75_ASYNC_STAGE   1 = dedicated stage thread (default), 0 = the
  #                         producer stages inline and the freed context
  #                         buys one more fold worker
  #   PRL_V75_HMX_CLOCK     PRL_HMX_CLOCK_POLICY override (default: keep
  #                         build.conf's value)
  V75_WORKERS="${PRL_V75_WORKERS:-2}"
  V75_ASYNC="${PRL_V75_ASYNC_STAGE:-1}"
  PRL_KERNEL_DEFINES=("${PRL_KERNEL_DEFINES[@]/-DPRL_GROUP_WORKERS=4/-DPRL_GROUP_WORKERS=$V75_WORKERS}")
  if [[ "$V75_ASYNC" != "1" ]]; then
    PRL_KERNEL_DEFINES=("${PRL_KERNEL_DEFINES[@]/-DPRL_ASYNC_STAGE=1/-DPRL_ASYNC_STAGE=0}")
  fi
  if [[ -n "${PRL_V75_HMX_CLOCK:-}" ]]; then
    PRL_KERNEL_DEFINES=("${PRL_KERNEL_DEFINES[@]/-DPRL_HMX_CLOCK_POLICY=*/-DPRL_HMX_CLOCK_POLICY=$PRL_V75_HMX_CLOCK}")
  fi
fi

ARCHNUM="${ARCH#v}"
if [[ ! -d "$SDK/rtos/qurt/computev$ARCHNUM" ]]; then
  echo "Hexagon SDK at $SDK has no computev$ARCHNUM QURT tree" >&2
  exit 1
fi
if [[ ! -f "$HK/lib/6.3.0.0/hexagon_toolv88_$ARCH/libhexkl_micro.a" ]]; then
  echo "HexKL addon has no hexagon_toolv88_$ARCH libhexkl_micro.a" >&2
  exit 1
fi

HEXFLAGS=(
  -O3 -fPIC -std=gnu99 -m$ARCH -mhvx -mhmx -mhvx-length=128B -G0
  -DPRL_DSP_ARCH_NUM=$ARCHNUM
  -fno-zero-initialized-in-bss -DUSE_OS_QURT
  -I"$SDK/incs/stddef" -I"$SDK/incs" -I"$SDK/rtos/qurt/computev$ARCHNUM/include/qurt"
  -I"$SDK/rtos/qurt/computev$ARCHNUM/include/posix" -I"$SDK/ipc/fastrpc/incs"
  -I"$SDK/ipc/fastrpc/remote/ship/inc" -I"$HK/include" -I"$UDMA/api" -I"$UDMA/inc"
)
LINKFLAGS=(-m$ARCH -mhvx -mhmx -mhvx-length=128B -G0 -fPIC
  -Wl,-Bsymbolic -Wl,--exclude-libs,ALL -shared)
# Static archives must follow the objects that reference them. Firmware does
# not supply HexKL: unresolved hexkl_* imports are a broken payload.
LINKLIBS=(-Wl,--start-group
  "$HK/lib/6.3.0.0/hexagon_toolv88_$ARCH/libhexkl_micro.a" -Wl,--end-group -lc)
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
  "$UDMA/src/dma_utils.c" "${LINKFLAGS[@]}" "${LINKLIBS[@]}" -o "$OUT/libprl_fused_fused_skel.so"

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
    "$BUILD/dsp/dma.o" "${LINKLIBS[@]}" -o "$2"
}

if (( ${#PRL_KERNEL_MODES[@]} == 0 )); then
  compile_kernel "$BUILD/dsp/kernel.o"
  link_kernel "$BUILD/dsp/kernel.o" "$OUT/$PRL_SKEL_LIB"
else
  for mode in "${PRL_KERNEL_MODES[@]}"; do
    compile_kernel "$BUILD/dsp/kernel_mode$mode.o" "-D${PRL_MODE_DEFINE}=$mode"
    if [[ "$mode" == "${PRL_PACKAGE_MODE:?}" ]]; then
      link_kernel "$BUILD/dsp/kernel_mode$mode.o" "$OUT/$PRL_SKEL_LIB"
    fi
  done
fi

python3 "$ROOT/scripts/check-dsp-elf.py" --arch "0x$ARCHNUM" \
  "$OUT/libprl_fused_fused_skel.so" "$OUT/$PRL_SKEL_LIB"

echo "== payload hashes =="
sha256sum "$OUT"/*.so
