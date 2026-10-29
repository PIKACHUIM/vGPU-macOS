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
# -DKERNEL is load-bearing, and -mkernel does NOT imply it.
#
# Kernel.framework/Headers/IOKit/IOTypes.h picks the width of these typedefs like this:
#
#   #if !defined(__arm__) && !defined(__i386__) && !(defined(__x86_64__) && !defined(KERNEL))
#   typedef IOPhysicalAddress64  IOPhysicalAddress;
#   #else
#   typedef IOPhysicalAddress32  IOPhysicalAddress;
#
# so on x86_64 without KERNEL, IOPhysicalAddress and IOByteCount silently become 32-bit.
# That changes the mangled name of every function taking one -- IODeviceMemory::withRange
# becomes __ZN14IODeviceMemory9withRangeEjj instead of ...Eyy -- and the failure surfaces
# far away, as a linker error claiming no kext exports the symbol. The real symbol lives in
# com.apple.kpi.iokit (Boot KC), so all that is needed is to ask for the right width:
#
#   kmutil libraries -p <kext> | grep withRange
#     __ZN14IODeviceMemory9withRangeEyy in BootKernelExtensions.kc: com.apple.kpi.iokit
clang -arch x86_64 -mkernel -fapple-kext -DKERNEL \
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

# Optimization silently deletes unreachable code, and the code it deletes looks exactly
# like code that was never written. On 2026-10-27 a bare `return` earlier in
# dxReadbackTest made the whole kDxVm3dmpTemplate block unreachable: the build succeeded,
# the kext loaded, and the boot quietly ran a different experiment (docs §24). Two boot
# cycles were spent reading logs for code that was not in the binary at all.
#
# So: every experiment that owns an IOLog banner declares that banner here, and a build
# without it fails. Add the marker when you add the experiment -- not after it misleads you.
echo "--- reachability markers ---"
MARKERS="TEMPLATE only mode
TEMPLATE entered
TEMPLATE DEFINE+BIND+12 COTABLES
SPLIT DX_DEFINE_CONTEXT"
MISSING=0
OLDIFS=$IFS
IFS='
'
for m in $MARKERS; do
    if grep -aq -- "$m" "$BUNDLE/Contents/MacOS/$NAME"; then
        echo "present: $m"
    else
        echo "MISSING: $m (unreachable or written out by the optimizer)" >&2
        MISSING=$((MISSING + 1))
    fi
done
# Stop banner: the last line of the experiment. Checked separately because the
# string is built by concatenation in the source ("TEMPLATE verdict ->"), so a
# plain substring grep cannot see it -- same reason the 10-28 run had to be read
# out of the log instead of trusted from the build.
STOP="DX manager still absent"
if grep -aq -- "$STOP" "$BUNDLE/Contents/MacOS/$NAME"; then
    echo "present: TEMPLATE verdict (stop banner)"
else
    echo "MISSING: TEMPLATE verdict stop banner" >&2
    MISSING=$((MISSING + 1))
fi
IFS=$OLDIFS
if [ "$MISSING" != "0" ]; then
    echo "FATAL: $MISSING reachability marker(s) absent -- refusing to ship this binary" >&2
    exit 1
fi

echo "BUNDLE=$BUNDLE"
