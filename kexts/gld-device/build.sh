#!/bin/bash
set -euo pipefail
# 从仓库中执行；只在传入的现有目录中生成独立测试件，不安装任何内容。
source_dir="$(cd "$(dirname "$0")" && pwd)"
output_dir="${1:?请传入已存在的输出目录}"
test -d "$output_dir"
common=(-std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow
        -Wstrict-prototypes -O2 -g -fvisibility=hidden -I"$source_dir")
sources=("$source_dir/gld_device.c" "$source_dir/../probes/gld_device_probe.c")
frameworks=(-framework IOKit -framework CoreFoundation)
clang "${common[@]}" "${sources[@]}" "${frameworks[@]}" -o "$output_dir/gld_device_probe"
clang "${common[@]}" -DGLD_DEVICE_OPEN_TYPE=0xffffffffU "${sources[@]}" \
    "${frameworks[@]}" -o "$output_dir/gld_device_probe_bad_type"
# 可分别运行：sudo 正常探针；非 root 正常探针 --expect-denied；
# sudo 失败构建 --expect-open-failure。失败构建不能用作设备实现。
