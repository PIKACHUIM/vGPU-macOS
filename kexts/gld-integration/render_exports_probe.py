"""本地 CPU probe；真实模块 + 独立 surface/device 替身，不安装、不联网。"""
import hashlib
import pathlib
import shlex
import subprocess
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parents[2]
K = ROOT / "kexts"
ZIG = ROOT / "out/.ziglang/ziglang/zig.exe"
SOURCES = [
    "gld-integration/render_exports.c", "gld-integration/render_exports_probe.c",
    "gld-integration/context_exports.c", "gld-context/vgpu_context.c",
    "gld-framebuffer/vgpu_framebuffer.c", "gld-state/vgpu_clear_state.c",
    "gld-texture/vgpu_texture.c", "gld-texture/vgpu_texture_bgra.c",
    "gld-objects/vgpu_objects.c",
]
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion",
         "-Wshadow", "-O1", "-g", "-target", "x86_64-windows-gnu", "-D__APPLE__=1"]
# __APPLE__ 仅让纯声明的 device 头可被 CPU fixture 使用；不链接 device 实现，
# 不代表 Windows 编译已经验证 macOS ABI。
for name, extra in [("strict", []), ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"]),
                    ("ndebug", ["-DNDEBUG"])]:
    exe = ROOT / "out" / ("gld-render-exports-" + name + ".exe")
    command = [str(ZIG), "cc", *FLAGS, *extra, "-I" + str(K / "gld-objects/cpu-fixture"),
               *(str(K / source) for source in SOURCES), "-lpthread", "-o", str(exe)]
    print("构建 CPU probe " + name, flush=True)
    subprocess.run(command, cwd=ROOT, check=True, timeout=180)
    subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=60)
print("本地 CPU 严格/UBSan/NDEBUG 全部通过；未覆盖 GPU/系统 GLD ABI。", flush=True)

# 显式选择已授权 VM；只链接 probe 的 surface/device 替身，不编译真实 GPU 后端。
if "--sdk" in sys.argv[1:]:
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(K / "gld-integration/.test-deps"))
    import mssh

    headers = ["gld-integration/render_exports.h", "gld-integration/context_exports.h",
               "gld-context/vgpu_context.h", "gld-framebuffer/vgpu_framebuffer.h",
               "gld-state/vgpu_clear_state.h", "gld-texture/vgpu_texture.h",
               "gld-texture/vgpu_texture_bgra.h", "gld-objects/vgpu_objects.h",
               "gld-device/gld_device.h", "gld-surface/vgpu_surface.h", "gld-pixel/gld_pixel.h"]
    snapshot = {name: (K / name).read_bytes() for name in SOURCES + headers}
    remote = "/var/tmp/render-exports-sdk-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
    client = mssh.connect(retries=1)

    def run(command):
        result = mssh.run(client, command, 180)
        if result:
            raise RuntimeError("SDK CPU 验证失败，exit=" + str(result))

    try:
        run("test -d /var/tmp && sw_vers && xcrun clang --version && xcrun --show-sdk-path")
        folders = sorted({name.split("/")[0] for name in snapshot})
        run("mkdir " + shlex.quote(remote) + " && mkdir " +
            " ".join(shlex.quote(remote + "/" + name) for name in folders))
        with client.open_sftp() as sftp:
            for name, content in snapshot.items():
                with sftp.open(remote + "/" + name, "wb") as stream:
                    stream.write(content)
                with sftp.open(remote + "/" + name, "rb") as stream:
                    if stream.read() != content:
                        raise RuntimeError("SDK 源码回读不一致：" + name)
                print("SHA256 " + hashlib.sha256(content).hexdigest() + " " + name, flush=True)
        flags = '-isysroot "$(xcrun --show-sdk-path)" -std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow -O1 -g'
        for name, extra in [("strict", ""), ("sanitized", "-fsanitize=address,undefined -fno-sanitize-recover=all"),
                            ("ndebug", "-DNDEBUG")]:
            executable = "render-exports-" + name
            print("构建并运行真实 SDK CPU probe " + name, flush=True)
            run("cd " + shlex.quote(remote) + " && xcrun clang " + flags + " " + extra + " " +
                " ".join(SOURCES) + " -pthread -o " + executable +
                " && ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./" + executable)
        changed = [name for name, content in snapshot.items() if (K / name).read_bytes() != content]
        if changed:
            raise RuntimeError("测试期间源码发生改变：" + ", ".join(changed))
        print("SDK CPU 严格/ASan+UBSan/NDEBUG 全部通过；平台不支持 LSan；无 GPU、安装或重启。快照目录：" + remote, flush=True)
    finally:
        client.close()
