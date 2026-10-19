#!/bin/sh
# Build the arm64 helper used by the Root backend.
# Requires ANDROID_NDK_HOME (or ANDROID_NDK) with a modern NDK.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}
if [ -z "$NDK" ]; then
  echo "ANDROID_NDK_HOME is not set" >&2
  exit 1
fi
CC=$(find "$NDK/toolchains/llvm/prebuilt" -name 'aarch64-linux-android26-clang' | head -n 1)
if [ -z "$CC" ]; then
  echo "aarch64-linux-android26-clang not found under $NDK" >&2
  exit 1
fi
OUT="$ROOT/assets/bin/arm64-v8a"
mkdir -p "$OUT"
"$CC" -O2 -Wall -Wextra -shared -fPIC -mbranch-protection=bti \
  -o "$OUT/libqpollhook.so" "$ROOT/native/qpoll_hook.c" -llog
"$CC" -O2 -Wall -Wextra -mbranch-protection=bti \
  -o "$OUT/qpoll-inject" "$ROOT/native/qpoll_inject.c" -ldl
chmod 0755 "$OUT/qpoll-inject"
echo "Built:"
ls -l "$OUT"
