#!/bin/sh
# Build the vgpuFramebuffer kext on the guest (x86_64).
#
#   sh build.sh          -> build/vgpuFramebuffer.kext
#   sudo sh install.sh   -> /Library/Extensions + staging
#
# Command Line Tools are enough: no Xcode, no KDK. Two details are load-bearing and are
# explained in docs/kext-loading.md:
#
#   * `-sectcreate __TEXT __info_plist` embeds the plist, which a kernel collection needs
#     because there is no separate Info.plist to read.
#   * `KMOD_EXPLICIT_DECL` in the source produces the `_kmod_info` symbol. The section
#     alone is not enough -- ld64 does not derive kmod_info from it.

set -eu

NAME=vgpuFramebuffer
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
# `-kext` was removed from clang; hand it to ld64 directly. Undefined symbols are expected
# and fine: the kext loader resolves them at load time against the loaded families
# (IOGraphicsFamily is already resident from the System KC).
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
    echo "OK: _kmod_info present"
else
    echo "WARNING: no _kmod_info symbol -- a kernel collection will refuse this bundle" >&2
fi

echo "--- undefined symbol count (resolved at load) ---"
nm -u "$BUNDLE/Contents/MacOS/$NAME" 2>/dev/null | wc -l

echo "BUNDLE=$BUNDLE"
