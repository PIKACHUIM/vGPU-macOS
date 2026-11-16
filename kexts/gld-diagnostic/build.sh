#!/bin/bash
set -euo pipefail
umask 022

# 输出必须是 /var/tmp 下尚不存在的目录；不安装、不签名、不修改系统设置。
src=$(cd "$(dirname "$0")" && pwd -P)
if [[ $# -ne 1 ]]; then
    printf '用法: bash %s /var/tmp/新建临时目录/build\n' "$0" >&2
    exit 2
fi
out=$1
case "$out" in
    /var/tmp/*|/private/var/tmp/*) ;;
    *) printf '构建输出仅允许在 /var/tmp 下\n' >&2; exit 2 ;;
esac
[[ -d "$(dirname "$out")" && ! -e "$out" && ! -L "$out" ]]
[[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]]
[[ $(sw_vers -buildVersion) == 24G419 ]]
mkdir "$out"
out=$(cd "$out" && pwd -P)
case "$out" in
    /private/var/tmp/*|/var/tmp/*) ;;
    *) printf '拒绝解析到临时目录之外的构建路径\n' >&2; exit 2 ;;
esac
bundle="$out/VGPUDriverDiagnostic.bundle"
exe="$bundle/Contents/MacOS/VGPUDriverDiagnostic"
mkdir -p "$bundle/Contents/MacOS"
cp "$src/Info.plist" "$bundle/Contents/Info.plist"
plutil -lint "$bundle/Contents/Info.plist"
[[ $(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$bundle/Contents/Info.plist") == VGPUDriverDiagnostic ]]
[[ $(/usr/libexec/PlistBuddy -c 'Print :CFBundleName' "$bundle/Contents/Info.plist") == VGPUDriverDiagnostic ]]
[[ $(/usr/libexec/PlistBuddy -c 'Print :CFBundlePackageType' "$bundle/Contents/Info.plist") == BNDL ]]

flags=(-arch x86_64 -mmacosx-version-min=15.0 -std=c11 -Wall -Wextra -Werror
       -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wshadow
       -O2 -fno-common -fvisibility=hidden)
xcrun clang --version
set -x
xcrun clang "${flags[@]}" -c "$src/diagnostic.c" -o "$out/diagnostic.o"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -c "$src/traps.S" -o "$out/traps.o"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -bundle -Wl,-fatal_warnings \
    "$out/diagnostic.o" "$out/traps.o" -o "$exe"
xcrun clang "${flags[@]}" "$src/../probes/gld_bundle_probe.c" -o "$out/gld_bundle_probe"
set +x

# 独立于 dlsym 核对二进制实际导出集合，不允许意外符号。
awk -F '[()]' '/^GLD_(IMPLEMENTED|TRAP)\(/ { print "_" $2 }' \
    "$src/required_entries.inc" > "$out/required.exports"
[[ $(wc -l < "$out/required.exports") -eq 129 ]]
{
    cat "$out/required.exports"
    printf '%s\n' _gldInitializeLibrary _gldTerminateLibrary
} | LC_ALL=C sort > "$out/expected.exports"
xcrun nm -gjU "$exe" | LC_ALL=C sort > "$out/actual.exports"
diff -u "$out/expected.exports" "$out/actual.exports"
file "$exe" "$out/gld_bundle_probe"
otool -L "$exe" > "$out/dependencies.txt"
cat "$out/dependencies.txt"
"$out/gld_bundle_probe" "$exe" 2>&1 | tee "$out/probe.log"
(
    cd "$out"
    shasum -a 256 VGPUDriverDiagnostic.bundle/Contents/Info.plist \
        VGPUDriverDiagnostic.bundle/Contents/MacOS/VGPUDriverDiagnostic \
        gld_bundle_probe actual.exports dependencies.txt probe.log > SHA256SUMS
    cat SHA256SUMS
)
printf 'BUILD_PASS bundle=%s executable=%s\n' "$bundle" "$exe"
