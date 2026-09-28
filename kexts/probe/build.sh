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
clang -arch x86_64 -nostdlib -mmacosx-version-min=11.0 \
      -I "$KERNEL_HEADERS" \
      -Wl,-kext \
      -o "$BUNDLE/Contents/MacOS/$NAME" "$OUT/$NAME.o"

echo "--- result ---"
ls -la "$BUNDLE/Contents/MacOS/$NAME"
file "$BUNDLE/Contents/MacOS/$NAME"
echo "BUNDLE=$BUNDLE"
