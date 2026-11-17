"""隔离真实 macOS SDK CPU 验证；不安装、不链接实际 device/surface、不执行 GPU。"""
import hashlib
import importlib.util
import shlex
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pipeline_probe_mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
remote = "/var/tmp/gi-pipeline-probe-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
files = [
    "gld-integration/context_exports.h", "gld-integration/context_exports.c",
    "gld-integration/pipeline_gate_probe.c", "gld-integration/context_exports_probe.c",
    "gld-device/gld_device.h", "gld-context/vgpu_context.h", "gld-context/vgpu_context.c",
    "gld-objects/vgpu_objects.h", "gld-objects/vgpu_objects.c",
    "gld-objects/cpu-fixture/surface_fixture.c", "gld-surface/vgpu_surface.h",
    "gld-texture/vgpu_texture.h", "gld-texture/vgpu_texture.c",
    "gld-texture/vgpu_texture_bgra.h", "gld-texture/vgpu_texture_bgra.c",
    "gld-pixel/gld_pixel.h", "gld-pixel/gld_pixel.c",
]
client = mssh.connect(retries=1)


def run(command):
    print("COMMAND " + command, flush=True)
    status = mssh.run(client, command, 120)
    if status:
        raise SystemExit(f"CPU probe 失败，状态 {status}；停止，不自动重试")


try:
    run("test -d /var/tmp && mkdir " + shlex.quote(remote))
    run("xcrun clang --version && xcrun --show-sdk-path")
    with client.open_sftp() as sftp:
        directories = set()
        for relative in files:
            current = remote
            for part in Path(relative).parts[:-1]:
                current += "/" + part
                if current not in directories:
                    sftp.mkdir(current)
                    directories.add(current)
            source = ROOT / "kexts" / relative
            payload = source.read_bytes()
            print("SHA256 " + hashlib.sha256(payload).hexdigest() + " " + relative, flush=True)
            with sftp.open(remote + "/" + relative, "wb") as target:
                target.write(payload)
    sources = [p for p in files if p.endswith(".c") and not p.endswith("context_exports_probe.c")]
    base = "-std=c11 -Wall -Wextra -Werror -Wpedantic -g -O1"
    for variant, flags in (
        ("strict", ""),
        ("ndebug", "-DNDEBUG"),
        ("ubsan", "-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"),
        ("asan", "-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"),
    ):
        binary = remote + "/pipeline-" + variant
        run("xcrun clang " + base + " " + flags + " " +
            " ".join(shlex.quote(remote + "/" + p) for p in sources) +
            " -lpthread -o " + shlex.quote(binary))
        run(shlex.quote(binary))
    # 现有 fixture 原样进行严格回归，不覆盖其内容或把 NDEBUG assert 当作测试。
    regression = [p.replace("pipeline_gate_probe.c", "context_exports_probe.c") for p in sources]
    run("xcrun clang " + base + " " +
        " ".join(shlex.quote(remote + "/" + p) for p in regression) +
        " -lpthread -o " + shlex.quote(remote + "/metadata-regression"))
    run(shlex.quote(remote + "/metadata-regression"))
    run("shasum -a 256 " + " ".join(shlex.quote(remote + "/" + p) for p in files))
    print("SHA256 " + hashlib.sha256(Path(__file__).read_bytes()).hexdigest() + " pipeline_gate_probe.py")
    print("隔离 SDK CPU 验证完成：" + remote)
finally:
    client.close()
