#!/bin/bash
set -euo pipefail
umask 022
src=$(cd "$(dirname "$0")" && pwd -P)
[[ $# -eq 1 || $# -eq 2 ]]
out=$1
experiment=0
if [[ $# -eq 2 ]]; then
  [[ $2 == --controlled-public-experiment ]]
  experiment=1
fi
case "$out" in /var/tmp/*|/private/var/tmp/*) ;; *) exit 2 ;; esac
[[ -d "$(dirname "$out")" && ! -e "$out" && ! -L "$out" ]]
[[ $(uname -s) == Darwin && $(uname -m) == x86_64 ]]
[[ $(sw_vers -buildVersion) == 24G419 ]]
mkdir "$out"
out=$(cd "$out" && pwd -P)
case "$out" in /var/tmp/*|/private/var/tmp/*) ;; *) exit 2 ;; esac
bundle="$out/VGPUDriverIntegration.bundle"
if [[ $experiment -eq 1 ]]; then
  bundle="$out/VGPUDriverControlledCandidate.bundle"
fi
exe="$bundle/Contents/MacOS/VGPUDriverIntegration"
mkdir -p "$bundle/Contents/MacOS"
cp "$src/Info.plist" "$bundle/Contents/Info.plist"
plutil -lint "$bundle/Contents/Info.plist"
# 从唯一原始129槽清单派生，避免复制清单漂移和真实入口与trap重复定义。
awk '
/^GLD_(IMPLEMENTED|TRAP)\(/ {
  name=$0; sub(/^[^(]*\(/,"",name); sub(/\).*/,"",name);
  implemented=(name ~ /^gld(GetVersion|CreateDevice|DestroyDevice|PopulateRendererInfo|ChoosePixelFormat|DestroyPixelFormat|CreateShareGroup|DestroyShareGroup|CreateContext|DestroyContext|PopulateContextDispatch|UpdateDispatch|UpdateDrawFramebuffer|UpdateReadFramebuffer|UpdateTransformFeedbackState|CreateTexture|ModifyTexture|DestroyTexture|CreateSampler|DestroySampler|CreatePipelineProgram|ModifyPipelineProgram|UnbindPipelineProgram|DestroyPipelineProgram|CreateProgram|DestroyProgram|CreateFramebuffer|DestroyFramebuffer|LoadFramebuffer|ClearFramebufferData|ReadFramebufferData|GetError|FlushContext|WaitForContext)$/);
  print (implemented ? "GLD_IMPLEMENTED(" : "GLD_TRAP(") name ")";
}' "$src/../gld-diagnostic/required_entries.inc" > "$out/integration_entries.inc"
[[ $(wc -l < "$out/integration_entries.inc") -eq 129 ]]
awk -F '[()]' '{print "_" $2}' "$out/integration_entries.inc" > "$out/expected.exports"
printf '%s\n' _gldInitializeLibrary _gldTerminateLibrary _vgpuIntegrationStatus _vgpuIntegrationPublishCandidate _vgpuIntegrationEnableMetadata _vgpuIntegrationEnableControlledDraw _vgpuIntegrationRead >> "$out/expected.exports"
LC_ALL=C sort -o "$out/expected.exports" "$out/expected.exports"
flags=(-arch x86_64 -mmacosx-version-min=15.0 -std=c11 -Wall -Wextra -Werror
       -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wshadow
       -O2 -g -fno-common -fvisibility=hidden)
# 候选宏统一覆盖所有生产单元，使 cap/renderer/context 的精确 PF 标志一致。
# 此构建允许受控公开实验自动启用；不代表通用驱动或 COMPLIANT/core 支持。
if [[ $experiment -eq 1 ]]; then flags+=(-DGI_CONTROLLED_PUBLIC_EXPERIMENT=1); fi
xcrun clang "${flags[@]}" -DgldInitializeLibrary=gi_device_initialize \
  -DgldTerminateLibrary=gi_device_terminate -DgldCreateDevice=gi_device_create \
  -DgldDestroyDevice=gi_device_destroy -DgldPopulateRendererInfo=gi_device_populate \
  -c "$src/../gld-device/gld_device.c" -o "$out/device.o"
xcrun clang "${flags[@]}" -DgldChoosePixelFormat=gi_pixel_choose \
  -DgldDestroyPixelFormat=gi_pixel_destroy \
  -c "$src/../gld-pixel/gld_pixel_exports.c" -o "$out/pixel_exports.o"
for name in gld_pixel gld_pixel_adapter; do
  xcrun clang "${flags[@]}" -c "$src/../gld-pixel/$name.c" -o "$out/$name.o"
done
metadata_objects=()
for unit in gld-context/vgpu_context gld-objects/vgpu_objects \
            gld-texture/vgpu_texture gld-texture/vgpu_texture_bgra gld-surface/vgpu_surface \
            gld-framebuffer/vgpu_framebuffer gld-state/vgpu_clear_state \
            gld-state/vgpu_triangle_state gld-integration/tcl_exports \
            gld-integration/dispatch_exports gld-integration/render_exports; do
  object="$out/${unit##*/}.o"
  xcrun clang "${flags[@]}" -c "$src/../$unit.c" -o "$object"
  metadata_objects+=("$object")
done
xcrun clang "${flags[@]}" -c "$src/context_exports.c" -o "$out/context_exports.o"
xcrun clang "${flags[@]}" -c "$src/integration.c" -o "$out/integration.o"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -I"$out" \
  -c "$src/traps.S" -o "$out/traps.o"
xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -bundle -Wl,-fatal_warnings \
  -Wl,-exported_symbols_list,"$out/expected.exports" \
  "$out/device.o" "$out/pixel_exports.o" "$out/gld_pixel.o" \
  "$out/gld_pixel_adapter.o" "$out/integration.o" "$out/context_exports.o" \
  "${metadata_objects[@]}" "$out/traps.o" \
  -framework IOKit -framework CoreFoundation -o "$exe"
xcrun clang "${flags[@]}" -I"$out" "$src/probe.c" \
  -framework IOKit -framework CoreFoundation -o "$out/integration_probe"
xcrun nm -gjU "$exe" | LC_ALL=C sort > "$out/actual.exports"
diff -u "$out/expected.exports" "$out/actual.exports"
otool -L "$exe" > "$out/dependencies.txt"
shasum -a 256 "$exe" "$out/integration_probe" > "$out/SHA256SUMS"
printf 'BUILD_PASS %s\n' "$exe"
# 不自动提权、安装、签名、调用CGL或提交GPU；探针由外部明确执行。
