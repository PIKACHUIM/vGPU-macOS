#!/bin/sh
# Build the probe kext with an experiment selected at compile time.
#
#   sh buildx.sh                    -> read-only snapshot (mode 0)
#   sh buildx.sh 1 1920 1080        -> write SVGA_REG_WIDTH/HEIGHT then snapshot
#   sh buildx.sh 2 1920 1080        -> write SVGA_REG_MAX_WIDTH/HEIGHT then snapshot
#
# The experiment has to be a compile-time constant: the whole point is that exactly one
# code path exists in the binary, so a `log show` line can never be ambiguous about which
# experiment produced it.

set -eu

MODE=${1:-0}
W=${2:-0}
H=${3:-0}

HERE=$(cd "$(dirname "$0")" && pwd)
NAME=vgpuProbe
OUT="$HERE/build"
BUNDLE="$OUT/$NAME.kext"

SDK=$(xcrun --show-sdk-path)
KERNEL_HEADERS="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

echo "--- experiment: mode=$MODE size=${W}x${H} ---"

rm -rf "$OUT"
mkdir -p "$BUNDLE/Contents/MacOS"
cp "$HERE/Info.plist" "$BUNDLE/Contents/Info.plist"

clang -arch x86_64 -mkernel -fapple-kext -DKERNEL \
      -fno-builtin -fno-rtti -fno-exceptions -fno-common \
      -fno-asynchronous-unwind-tables -fno-stack-protector \
      -mmacosx-version-min=11.0 \
      -I "$KERNEL_HEADERS" \
      -DVGPU_PROBE_MODE="$MODE" \
      -DVGPU_PROBE_NEW_WIDTH="$W" \
      -DVGPU_PROBE_NEW_HEIGHT="$H" \
      -c "$HERE/$NAME.cpp" -o "$OUT/$NAME.o"

clang -arch x86_64 -nostdlib -mmacosx-version-min=11.0 \
      -I "$KERNEL_HEADERS" \
      -Wl,-kext \
      -Wl,-sectcreate,__TEXT,__info_plist,"$BUNDLE/Contents/Info.plist" \
      -o "$BUNDLE/Contents/MacOS/$NAME" "$OUT/$NAME.o"

ls -la "$BUNDLE/Contents/MacOS/$NAME"
if nm -a "$BUNDLE/Contents/MacOS/$NAME" 2>/dev/null | grep -q _kmod_info; then
    echo "OK: _kmod_info present"
else
    echo "WARNING: no _kmod_info symbol" >&2
fi
echo "BUNDLE=$BUNDLE"
