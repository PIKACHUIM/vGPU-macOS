"""真实 macOS SDK 编译/CPU 探针；只读核验已知 producer，不安装、不调用 GLD/GPU。

已证版本：macOS 15.7.3 / 24G419，x86_64。
GLEngine UUID FB994F78-34C6-39F3-857D-99F9B3013E8F。
libGFXShared UUID CEB3FF13-179A-3AFD-AB07-31BE4C2073B9。
gleBindRenderbuffer 454617: flags=0x2002 -> gleCreateTextureObject。
37e0bc..37e0e3: kind=flags&15，kind2 的 groups=levels=1。
37e0e7..37e0fb: malloc(0x3c0 + groups*levels*0x20)=0x3e0。
37e143/147/152: gfxInitializeGLTexture(flags,groups,levels,object+0x28)。
174c53: 保存 flags 到 (object+0x28)+0x2e8，即 TD=object+0x310。
37e1b0 -> gfxCreatePluginTexture；175215..175256: TD=texture+0x2e8。
因此 Create 时就有 kind2/renderbuffer 标志，TD 尾部容量为 0xd0；
L=TD+0xb0，最后 CPU 指针为 +0xc8..+0xcf。不是 Modify 后猜测升级。
默认 flags=0x1000|i，尤其 0x1002 保持 metadata 0x80。

原始交叉证据：out/gld-context-init-required-verified.txt 第5节；
out/gld-texture-verified-snippets.txt [B][H1]；out/gfx-loader.txt。
客体只读原始指令来自既有 /var/tmp/vgpu-fb/glengine-code.txt。
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import shlex
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("texture_views_mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
remote = "/var/tmp/gi-texture-views-" + stamp
local = ROOT / "out" / ("texture-system-views-" + stamp)
files = [
    "gld-integration/context_exports.h", "gld-integration/context_exports.c",
    "gld-integration/texture_system_views_probe.c", "gld-device/gld_device.h",
    "gld-context/vgpu_context.h", "gld-context/vgpu_context.c",
    "gld-objects/vgpu_objects.h", "gld-objects/vgpu_objects.c",
    "gld-objects/cpu-fixture/surface_fixture.c", "gld-surface/vgpu_surface.h",
    "gld-texture/vgpu_texture.h", "gld-texture/vgpu_texture.c",
    "gld-texture/vgpu_texture_bgra.h", "gld-texture/vgpu_texture_bgra.c",
    "gld-pixel/gld_pixel.h",
]
if not local.parent.is_dir():
    raise RuntimeError("out 父目录不存在")
local.mkdir()
paths = {"kexts/" + name: ROOT / "kexts" / name for name in files}
paths["kexts/gld-integration/texture_system_views_probe.py"] = Path(__file__)
paths["tools/mssh.py"] = ROOT / "tools/mssh.py"
snapshot = {name: path.read_bytes() for name, path in paths.items()}
manifest = {name: hashlib.sha256(data).hexdigest() for name, data in snapshot.items()}
(local / "source-sha256.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
log = (local / "build.log").open("w", encoding="utf-8", buffering=1)
client = None
passed = []
binary_hashes = {}
status = 1

def record(text):
    print(text, flush=True)
    log.write(text + "\n")

def run(command):
    record("COMMAND " + command)
    capture = io.StringIO()
    with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
        code = mssh.run(client, command, 120)
    text = capture.getvalue()
    record(text)
    record("EXIT " + str(code))
    if code:
        raise RuntimeError(f"探针失败，状态 {code}；停止，不自动重试")
    return text

def unchanged():
    changed = [name for name, path in paths.items() if path.read_bytes() != snapshot[name]]
    record("WORKSPACE_CHANGED " + json.dumps(changed))
    if changed:
        raise RuntimeError("源码已变化；本次结果不能代表当前工作区，需重新验证")

def verify_remote():
    text = run("cd " + shlex.quote(remote) + " && shasum -a 256 " +
               " ".join(shlex.quote(name) for name in manifest))
    observed = dict((name, digest) for digest, name in
                    (line.split(None, 1) for line in text.splitlines()))
    if observed != manifest:
        raise RuntimeError("远端源码与固定快照哈希不匹配")
    record("REMOTE_SOURCE_HASHES_PASS " + str(len(manifest)))

try:
    unchanged()
    client = mssh.connect(retries=1)
    run("sw_vers && uname -m && xcrun --show-sdk-path && xcrun --show-sdk-version && xcrun clang --version")
    identity = run("sw_vers -buildVersion && uname -m && "
        "/usr/bin/dyld_info -arch x86_64 -uuid /System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLEngine.bundle/GLEngine && "
        "/usr/bin/dyld_info -arch x86_64 -uuid /System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGFXShared.dylib")
    for expected in ("24G419", "x86_64", "FB994F78-34C6-39F3-857D-99F9B3013E8F",
                     "CEB3FF13-179A-3AFD-AB07-31BE4C2073B9"):
        if expected.lower() not in identity.lower():
            raise RuntimeError("已核验 producer 身份不匹配：" + expected)
    evidence = '''import pathlib
lines = pathlib.Path("/var/tmp/vgpu-fb/glengine-code.txt").read_text().splitlines()
for line in lines:
    if line.startswith("0x"):
        addr = line.split()[0]
        if "0x7FFB0637E0B2" <= addr <= "0x7FFB0637E1B0" or "0x7FFB06454617" <= addr <= "0x7FFB0645461F":
            print(line)
'''
    run("python3 -c " + shlex.quote(evidence))
    run("test -d /var/tmp && mkdir " + shlex.quote(remote))
    with client.open_sftp() as sftp:
        directories = set()
        for relative, data in snapshot.items():
            current = remote
            for part in Path(relative).parts[:-1]:
                current += "/" + part
                if current not in directories:
                    sftp.mkdir(current)
                    directories.add(current)
            sftp.putfo(io.BytesIO(data), remote + "/" + relative)
    verify_remote()
    unchanged()
    sources = " ".join(shlex.quote(remote + "/kexts/" + p) for p in files if p.endswith(".c"))
    base = "xcrun clang -std=c11 -Wall -Wextra -Werror -Wpedantic -g "
    for name, flags in [
        ("strict", "-O2"),
        ("ndebug", "-O2 -DNDEBUG"),
        ("sanitizers", "-O1 -fsanitize=address,undefined -fno-omit-frame-pointer"),
    ]:
        executable = shlex.quote(remote + "/texture-views-" + name)
        run(base + flags + " " + sources + " -lpthread -o " + executable)
        run("ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 " + executable)
        binary_hashes[name] = run("shasum -a 256 " + executable).split()[0]
        passed.append(name)
        record("PASS " + name)
    verify_remote()
    unchanged()
    status = 0
    record("真实 SDK / CPU guard-pages 探针通过：" + remote)
except (Exception, SystemExit) as exc:
    record("FAILED " + type(exc).__name__)
    if isinstance(exc, RuntimeError):
        record(str(exc))
finally:
    (local / "result.json").write_text(json.dumps({
        "status": status, "remote": remote, "manifest": manifest,
        "passed": passed, "binary_sha256": binary_hashes,
        "gpu_executed": False, "fixture_only": True,
    }, indent=2, ensure_ascii=False), encoding="utf-8")
    record("FINAL " + str(status) + " " + str(local))
    log.close()
    if client:
        client.close()
sys.exit(status)
