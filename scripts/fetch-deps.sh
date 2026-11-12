#!/usr/bin/env bash
#   Hexagon SDK : community CI mirror on GitHub (overridable via HEXAGON_SDK_URL)
#   Android NDK : dl.google.com (overridable via ANDROID_NDK_URL)
#   HexKL addon : Qualcomm software center direct link (overridable via HEXKL_URL)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${PRL_DEPS_DIR:-$ROOT/deps}"
mkdir -p "$DEPS"

HEXAGON_SDK_URL_DEFAULT="https://github.com/snapdragon-toolchain/hexagon-sdk/releases/download/v6.3.0/hexagon-sdk-v6.3.0-amd64-lnx.tar.xz"
ANDROID_NDK_URL_DEFAULT="https://dl.google.com/android/repository/android-ndk-r27c-linux.zip"
HEXKL_URL_DEFAULT="https://softwarecenter.qualcomm.com/api/download/software/tools/Hexagon_KL/Linux/Debian/1.0.0-beta2/hexkl-1.0-beta.2.zip"

fetch() { # $1=url $2=output ; browser UA because the software center rejects curl's default
  if [[ -s "$2" ]]; then echo "have  $2"; return; fi
  echo "fetch $1"
  curl -fL --retry 3 -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36" -o "$2.part" "$1" && mv "$2.part" "$2"
}

if [[ -n "${HEXAGON_SDK_ROOT:-}" && -d "${HEXAGON_SDK_ROOT:-}" ]]; then
  echo "using existing HEXAGON_SDK_ROOT=$HEXAGON_SDK_ROOT"
elif [[ -d "$DEPS/hexagon-sdk/6.3.0.0" ]]; then
  echo "using existing $DEPS/hexagon-sdk/6.3.0.0"
else
  fetch "${HEXAGON_SDK_URL:-$HEXAGON_SDK_URL_DEFAULT}" "$DEPS/hexagon-sdk.tar.xz"
  mkdir -p "$DEPS/hexagon-sdk"
  tar -xf "$DEPS/hexagon-sdk.tar.xz" -C "$DEPS/hexagon-sdk"
fi

if [[ -n "${ANDROID_NDK_ROOT:-}" && -d "${ANDROID_NDK_ROOT:-}" ]]; then
  echo "using existing ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT"
elif [[ -d "$DEPS/android-ndk-r27c" ]]; then
  echo "using existing $DEPS/android-ndk-r27c"
else
  fetch "${ANDROID_NDK_URL:-$ANDROID_NDK_URL_DEFAULT}" "$DEPS/android-ndk.zip"
  unzip -q "$DEPS/android-ndk.zip" -d "$DEPS"
fi

HEXKL_URL="${HEXKL_URL:-$HEXKL_URL_DEFAULT}"
if [[ -n "${HEXKL_ROOT:-}" && -d "${HEXKL_ROOT:-}" ]]; then
  echo "using existing HEXKL_ROOT=$HEXKL_ROOT"
elif [[ -d "$DEPS/hexkl/hexkl_addon" ]]; then
  echo "using existing $DEPS/hexkl/hexkl_addon"
else
  mkdir -p "$DEPS/hexkl"
  case "$HEXKL_URL" in
    *.zip)
      fetch "$HEXKL_URL" "$DEPS/hexkl/hexkl.zip"
      unzip -q "$DEPS/hexkl/hexkl.zip" -d "$DEPS/hexkl"
      [[ -d "$DEPS/hexkl/hexkl_addon" ]] || { echo "zip must contain hexkl_addon/" >&2; exit 1; }
      ;;
    git+*)
      git clone "${HEXKL_URL#git+}" "$DEPS/hexkl/hexkl_addon"
      ;;
    *)
      echo "HEXKL_URL must be a .zip URL or git+<repo-url>" >&2
      exit 1
      ;;
  esac
fi

echo "deps ready under $DEPS"
