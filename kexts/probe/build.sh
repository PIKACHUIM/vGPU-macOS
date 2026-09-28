#!/bin/sh
# Build the minimal unsigned probe kext on the guest (x86_64).
#
# No Xcode project: Command Line Tools ship everything a kext build needs
# (clang, the macOS SDK, and the Kernel framework headers).
#
# Usage: sh build.sh            -> build/vgpuProbe.kext

set -eu

NAME=vgpuProbe
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/build"
BUNDLE="$OUT/$NAME.kext"

SDK=$(xcrun --show-sdk-path)
KERNEL_HEADERS="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

if [ ! -d "$KERNEL_HEADERS" ]; then
    echo "Kernel framework headers missing at $KERNEL_HEADERS" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$BUNDLE/Contents/MacOS"
cp "$HERE/Info.plist" "$BUNDLE/Contents/Info.plist"

echo "--- compiling ---"
clang -arch x86_64 -mkernel -fapple-kext \
      -fno-builtin -fno-rtti -fno-exceptions -fno-common \
      -fno-asynchronous-unwind-tables -fno-stack-protector \
      -mmacosx-version-min=11.0 \
      -I "$KERNEL_HEADERS" \
      -c "$HERE/$NAME.cpp" -o "$OUT/$NAME.o"

echo "--- linking ---"
# Modern clang no longer accepts `-kext` directly; pass it through to ld64.
#
# `-sectcreate __TEXT __info_plist` is not optional: ld64 synthesises the
# `_kmod_info` symbol (name + version + start/stop) *from the embedded plist*.
# Without it the bundle links fine but `kmutil load` rejects it with
#   "kexts must have a _kmod_info symbol"
# because a kernel collection has no separate Info.plist to read.
clang -arch x86_64 -nostdlib -mmacosx-version-min=11.0 \
      -I "$KERNEL_HEADERS" \
      -Wl,-kext \
      -Wl,-sectcreate,__TEXT,__info_plist,"$BUNDLE/Contents/Info.plist" \
      -o "$BUNDLE/Contents/MacOS/$NAME" "$OUT/$NAME.o"

echo "--- result ---"
ls -la "$BUNDLE/Contents/MacOS/$NAME"
file "$BUNDLE/Contents/MacOS/$NAME"

echo "--- kmod check ---"
if nm -a "$BUNDLE/Contents/MacOS/$NAME" 2>/dev/null | grep -q _kmod_info; then
    nm -a "$BUNDLE/Contents/MacOS/$NAME" | grep _kmod_info
    echo "OK: _kmod_info present"
else
    echo "WARNING: no _kmod_info symbol -- a kernel collection will refuse this bundle" >&2
    nm -a "$BUNDLE/Contents/MacOS/$NAME" | grep -E ' [TtDd] ' | head -10
fi
echo "--- embedded plist section ---"
otool -l "$BUNDLE/Contents/MacOS/$NAME" | grep -A3 info_plist | head -8
echo "BUNDLE=$BUNDLE"
