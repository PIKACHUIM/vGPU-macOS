"""系统 read/error 的真实 SDK 隔离 CPU 回归；不连接设备、不执行 GPU。"""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import sys
import tarfile
from datetime import datetime, timezone

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
local = ROOT / "out" / ("integration-read-sdk-" + stamp)
assert local.parent.is_dir()
local.mkdir()
remote = "/var/tmp/integration-read-sdk-" + stamp
modules = ["gld-integration", "gld-device", "gld-pixel", "gld-context", "gld-objects",
           "gld-texture", "gld-framebuffer", "gld-state", "gld-surface", "gld-diagnostic"]
manifest = {}
with tarfile.open(local / "source.tar", "w") as archive:
    for module in modules:
        for path in sorted((ROOT / "kexts" / module).rglob("*")):
            if not path.is_file() or path.suffix not in (".h", ".c", ".S", ".sh", ".plist", ".inc"):
                continue
            if "validation-" in str(path):
                continue
            rel = path.relative_to(ROOT).as_posix()
            data = path.read_bytes()
            manifest[rel] = hashlib.sha256(data).hexdigest()
            entry = tarfile.TarInfo(rel)
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
(local / "source-sha256.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
(local / "remote.txt").write_text(remote + "\n", encoding="utf-8")
client = mssh.connect(retries=1)
log = []

def run(command):
    log.append("COMMAND " + command)
    stdin, stdout, stderr = client.exec_command(command, timeout=120)
    stdin.close()
    text = stdout.read().decode("utf-8", "replace") + stderr.read().decode("utf-8", "replace")
    status = stdout.channel.recv_exit_status()
    log.extend([text, "EXIT " + str(status)])
    print(text, end="", flush=True)
    if status:
        raise RuntimeError("隔离验证失败，见 validation.log")

try:
    run("test -d /var/tmp && mkdir " + shlex.quote(remote))
    with client.open_sftp() as sftp:
        sftp.put(str(local / "source.tar"), remote + "/source.tar")
    run(f"tar -xf {remote}/source.tar -C {remote} && sw_vers && xcrun clang --version")
    run(f"bash {remote}/kexts/gld-integration/build.sh {remote}/build")
    exe = remote + "/build/VGPUDriverIntegration.bundle/Contents/MacOS/VGPUDriverIntegration"
    run(f"{remote}/build/integration_probe {exe} --empty")
    sources = ["gld-integration/integration.c", "gld-integration/context_exports.c",
               "gld-integration/render_exports.c", "gld-context/vgpu_context.c",
               "gld-objects/vgpu_objects.c", "gld-texture/vgpu_texture.c",
               "gld-texture/vgpu_texture_bgra.c", "gld-framebuffer/vgpu_framebuffer.c",
               "gld-state/vgpu_clear_state.c", "gld-state/vgpu_triangle_state.c",
               "gld-integration/tcl_exports.c", "gld-integration/dispatch_exports.c",
               "gld-pixel/gld_pixel.c"]
    flags = "-arch x86_64 -mmacosx-version-min=15.0 -std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -O1 -g"
    for mode, extra in [("strict", ""), ("ndebug", "-DNDEBUG"),
                        ("ubsan", "-fsanitize=undefined -fno-sanitize-recover=all"),
                        ("asan", "-fsanitize=address")]:
        inputs = " ".join(remote + "/kexts/" + s for s in sources)
        binary = remote + "/read-" + mode
        run(f"xcrun clang {flags} {extra} {inputs} {remote}/kexts/gld-integration/integration_read_test.c -o {binary} && ASAN_OPTIONS=detect_leaks=0 {binary} && shasum -a 256 {binary} && otool -L {binary}")
    inputs = " ".join(remote + "/kexts/" + s for s in sources)
    # 既有 fixture 无完整原型；单独编译，不降低正式模块警告级别。
    run(f"xcrun clang -std=c11 -Wall -Wextra -Werror -c {remote}/kexts/gld-objects/cpu-fixture/surface_fixture.c -o {remote}/fixture.o")
    run(f"xcrun clang {flags} {inputs} {remote}/kexts/gld-integration/integration_metadata_test.c {remote}/fixture.o -o {remote}/metadata && {remote}/metadata")
    print("PASS " + str(local), flush=True)
finally:
    (local / "validation.log").write_text("\n".join(log), encoding="utf-8")
    client.close()
