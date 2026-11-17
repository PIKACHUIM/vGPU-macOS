"""隔离 macOS SDK/ASan/UBSan 编译；仅 CPU 生命周期，不安装或运行 GPU 操作。"""
import importlib.util
import shlex
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gi_probe_mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
remote = "/var/tmp/gi-context-probe-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
files = [
    "gld-integration/context_exports.h", "gld-integration/context_exports.c",
    "gld-integration/context_exports_probe.c", "gld-device/gld_device.h",
    "gld-context/vgpu_context.h", "gld-context/vgpu_context.c",
    "gld-objects/vgpu_objects.h", "gld-objects/vgpu_objects.c",
    "gld-objects/cpu-fixture/surface_fixture.c", "gld-surface/vgpu_surface.h",
    "gld-texture/vgpu_texture.h", "gld-texture/vgpu_texture.c",
    "gld-texture/vgpu_texture_bgra.h", "gld-texture/vgpu_texture_bgra.c",
    "gld-pixel/gld_pixel.h", "gld-pixel/gld_pixel.c",
]
client = mssh.connect(retries=1)

def run(command):
    status = mssh.run(client, command, 120)
    if status:
        raise SystemExit(f"隔离 probe 失败，状态 {status}；停止，不自动重试")

try:
    run("test -d /var/tmp && mkdir " + shlex.quote(remote))
    with client.open_sftp() as sftp:
        directories = set()
        for relative in files:
            parts = Path(relative).parts[:-1]
            current = remote
            for part in parts:
                current += "/" + part
                if current not in directories:
                    sftp.mkdir(current)
                    directories.add(current)
            sftp.put(str(ROOT / "kexts" / relative), remote + "/" + relative)
    sources = [p for p in files if p.endswith(".c")]
    flags = "-std=c11 -Wall -Wextra -Werror -Wpedantic -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
    command = "xcrun clang " + flags + " " + " ".join(shlex.quote(remote + "/" + p) for p in sources)
    command += " -lpthread -o " + shlex.quote(remote + "/context-probe")
    run(command)
    run(shlex.quote(remote + "/context-probe"))
    print("隔离 SDK/ASan/UBSan probe 完成：" + remote)
finally:
    client.close()
