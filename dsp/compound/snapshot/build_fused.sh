#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
SDK="${HEXAGON_SDK_ROOT:-$ROOT/hexsdk-dl/6.3.0.0}"
NDK="${ANDROID_NDK_ROOT:-$ROOT/ndk-dl/android-ndk-r27c}"
HK="${HEXKL_ROOT:-$ROOT/hexkl-beta2/hexkl_addon}"
UDMA="$SDK/addons/compute/libs/userdma/utils_lib"
HEX="$SDK/tools/HEXAGON_Tools/19.0.04/Tools/bin/hexagon-clang"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"
PRL_SPECIALIZE_HOT="${PRL_SPECIALIZE_HOT:-1}"
PRL_SPECIALIZE_QUEUE="${PRL_SPECIALIZE_QUEUE:-$PRL_SPECIALIZE_HOT}"
PRL_WAIT_FASTPATH="${PRL_WAIT_FASTPATH:-1}"
PRL_ASYNC_STAGE="${PRL_ASYNC_STAGE:-$PRL_SPECIALIZE_HOT}"
PRL_MM4_POINTER="${PRL_MM4_POINTER:-0}"
PRL_ADAPTIVE_SHARED="${PRL_ADAPTIVE_SHARED:-0}"
PRL_COMPACT_VTCM="${PRL_COMPACT_VTCM:-2}"
PRL_HMX_CLOCK_POLICY="${PRL_HMX_CLOCK_POLICY:-0}"
PRL_AUTO_WORKERS="${PRL_AUTO_WORKERS:-0}"
case "$PRL_AUTO_WORKERS" in
  0|1) ;;
  *) printf 'PRL_AUTO_WORKERS must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_HMX_CLOCK_POLICY" in
  0|1|2|3|4) ;;
  *) printf 'PRL_HMX_CLOCK_POLICY must be 0, 1, 2, 3 or 4\n' >&2; exit 2 ;;
esac
if [[ "$PRL_SPECIALIZE_HOT" == 1 && "$PRL_SPECIALIZE_QUEUE" == 1 ]]; then
  PRL_DEFAULT_SNAPSHOT_GROUP=16
else
  PRL_DEFAULT_SNAPSHOT_GROUP=1
fi
PRL_SNAPSHOT_GROUP="${PRL_SNAPSHOT_GROUP:-$PRL_DEFAULT_SNAPSHOT_GROUP}"
PRL_GROUP_WORKERS="${PRL_GROUP_WORKERS:-0}"
PRL_FUSED_DSP_OUT="${PRL_FUSED_DSP_OUT:-out/dsp/libprl_fused_fused_skel.so}"
PRL_FUSED_BUILD_ARM="${PRL_FUSED_BUILD_ARM:-1}"
case "$PRL_SNAPSHOT_GROUP" in
  1|2|4|8|16) ;;
  *) printf 'PRL_SNAPSHOT_GROUP must be 1, 2, 4, 8 or 16\n' >&2; exit 2 ;;
esac
case "$PRL_GROUP_WORKERS" in
  0|2|3|4) ;;
  *) printf 'PRL_GROUP_WORKERS must be 0, 2, 3 or 4\n' >&2; exit 2 ;;
esac
case "$PRL_FUSED_BUILD_ARM" in
  0|1) ;;
  *) printf 'PRL_FUSED_BUILD_ARM must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_COMPACT_VTCM" in
  0|1|2) ;;
  *) printf 'PRL_COMPACT_VTCM must be 0, 1 or 2\n' >&2; exit 2 ;;
esac
case "$PRL_ADAPTIVE_SHARED" in
  0|1) ;;
  *) printf 'PRL_ADAPTIVE_SHARED must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_MM4_POINTER" in
  0|1) ;;
  *) printf 'PRL_MM4_POINTER must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_ASYNC_STAGE" in
  0|1) ;;
  *) printf 'PRL_ASYNC_STAGE must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_SPECIALIZE_HOT" in
  0|1) ;;
  *) printf 'PRL_SPECIALIZE_HOT must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_WAIT_FASTPATH" in
  0|1) ;;
  *) printf 'PRL_WAIT_FASTPATH must be 0 or 1\n' >&2; exit 2 ;;
esac
case "$PRL_SPECIALIZE_QUEUE" in
  0|1) ;;
  *) printf 'PRL_SPECIALIZE_QUEUE must be 0 or 1\n' >&2; exit 2 ;;
esac
if [[ "$PRL_SPECIALIZE_QUEUE" == 1 && "$PRL_SPECIALIZE_HOT" != 1 ]]; then
  printf 'PRL_SPECIALIZE_QUEUE=1 requires PRL_SPECIALIZE_HOT=1\n' >&2
  exit 2
fi
if [[ "$PRL_ASYNC_STAGE" == 1 && "$PRL_SPECIALIZE_HOT" != 1 ]]; then
  printf 'PRL_ASYNC_STAGE=1 requires PRL_SPECIALIZE_HOT=1\n' >&2
  exit 2
fi
mkdir -p gen_fused out/dsp out/arm "$(dirname "$PRL_FUSED_DSP_OUT")"
"$SDK/ipc/fastrpc/qaic/bin/qaic" fused.idl -o gen_fused
"$HEX" -O3 -fPIC -std=gnu99 -mv79 -mhvx -mhmx -mhvx-length=128B -G0 -fno-zero-initialized-in-bss -DUSE_OS_QURT -DPRL_SPECIALIZE_HOT="$PRL_SPECIALIZE_HOT" -DPRL_SPECIALIZE_QUEUE="$PRL_SPECIALIZE_QUEUE" -DPRL_WAIT_FASTPATH="$PRL_WAIT_FASTPATH" -DPRL_ASYNC_STAGE="$PRL_ASYNC_STAGE" -DPRL_MM4_POINTER="$PRL_MM4_POINTER" -DPRL_ADAPTIVE_SHARED="$PRL_ADAPTIVE_SHARED" -DPRL_COMPACT_VTCM="$PRL_COMPACT_VTCM" -DPRL_SNAPSHOT_GROUP="$PRL_SNAPSHOT_GROUP" -DPRL_GROUP_WORKERS="$PRL_GROUP_WORKERS" -DPRL_HMX_CLOCK_POLICY="$PRL_HMX_CLOCK_POLICY" -DPRL_AUTO_WORKERS="$PRL_AUTO_WORKERS" \
  -I"$SDK/incs/stddef" -I"$SDK/incs" -I"$SDK/rtos/qurt/computev79/include/qurt" -I"$SDK/rtos/qurt/computev79/include/posix" \
  -I"$SDK/ipc/fastrpc/incs" -I"$SDK/ipc/fastrpc/remote/ship/inc" -Igen_fused -I"$HK/include" -I"$UDMA/api" -I"$UDMA/inc" \
  gen_fused/prl_fused_fused_skel.c fused_impl.c "$UDMA/src/dma_utils.c" \
  -Wl,--start-group "$HK/lib/6.3.0.0/hexagon_toolv88_v79/libhexkl_micro.a" -Wl,--end-group -lc \
  -shared -o "$PRL_FUSED_DSP_OUT"
if [[ "$PRL_FUSED_BUILD_ARM" == 1 ]]; then
"$CC" -O3 -std=gnu99 -I"$SDK/incs" -I"$SDK/incs/stddef" -I"$SDK/ipc/fastrpc/incs" \
  -I"$SDK/ipc/fastrpc/remote/ship/inc" -I"$SDK/ipc/fastrpc/rpcmem/inc" -Igen_fused \
  gen_fused/prl_fused_fused_stub.c fused_driver.c \
  -L"$SDK/ipc/fastrpc/remote/ship/android_aarch64" -lcdsprpc \
  "$SDK/ipc/fastrpc/rpcmem/prebuilt/android_aarch64/rpcmem.a" -ldl -lm -o out/arm/fused_driver
fi
printf 'FUSED_BUILD_OK hot=%s queue=%s wait_fastpath=%s async_stage=%s mm4_pointer=%s adaptive_shared=%s compact_vtcm=%s snapshot_group=%s group_workers=%s hmx_clock_policy=%s auto_workers=%s dsp_out=%s build_arm=%s\n' "$PRL_SPECIALIZE_HOT" "$PRL_SPECIALIZE_QUEUE" "$PRL_WAIT_FASTPATH" "$PRL_ASYNC_STAGE" "$PRL_MM4_POINTER" "$PRL_ADAPTIVE_SHARED" "$PRL_COMPACT_VTCM" "$PRL_SNAPSHOT_GROUP" "$PRL_GROUP_WORKERS" "$PRL_HMX_CLOCK_POLICY" "$PRL_AUTO_WORKERS" "$PRL_FUSED_DSP_OUT" "$PRL_FUSED_BUILD_ARM"
