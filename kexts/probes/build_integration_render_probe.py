"""隔离真实 SDK 构建与 CPU 检查；绝不安装 bundle、提权或执行 GPU。"""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
BUNDLE_INPUTS = (
    "gld-integration/integration.c", "gld-integration/integration.h",
    "gld-integration/context_exports.c", "gld-integration/context_exports.h",
    "gld-integration/render_exports.c", "gld-integration/render_exports.h",
    "gld-integration/tcl_exports.c", "gld-integration/tcl_exports.h",
    "gld-integration/dispatch_exports.c", "gld-integration/dispatch_exports.h",
    "gld-integration/render_exports_probe.c", "gld-integration/integration_read_test.c",
    "gld-integration/probe.c", "gld-integration/traps.S",
    "gld-integration/build.sh", "gld-integration/Info.plist",
    "gld-device/gld_device.c", "gld-device/gld_device.h",
    "gld-pixel/gld_pixel.c", "gld-pixel/gld_pixel.h",
    "gld-pixel/gld_pixel_adapter.c", "gld-pixel/gld_pixel_adapter.h",
    "gld-pixel/gld_pixel_exports.c", "gld-pixel/gld_pixel_exports.h",
    "gld-context/vgpu_context.c", "gld-context/vgpu_context.h",
    "gld-objects/vgpu_objects.c", "gld-objects/vgpu_objects.h",
    "gld-texture/vgpu_texture.c", "gld-texture/vgpu_texture.h",
    "gld-texture/vgpu_texture_bgra.c", "gld-texture/vgpu_texture_bgra.h",
    "gld-framebuffer/vgpu_framebuffer.c", "gld-framebuffer/vgpu_framebuffer.h",
    "gld-state/vgpu_clear_state.c", "gld-state/vgpu_clear_state.h",
    "gld-state/vgpu_triangle_state.c", "gld-state/vgpu_triangle_state.h",
    "gld-surface/vgpu_surface.c", "gld-surface/vgpu_surface.h",
    "gld-diagnostic/required_entries.inc",
)
PROBE = "kexts/probes/gld_integration_render_probe.c"
RUNNER = "kexts/probes/build_integration_render_probe.py"


def source_paths(reuse=False):
    paths = {ROOT / PROBE, ROOT / RUNNER, ROOT / "tools/mssh.py"}
    if reuse:
        pending = [ROOT / PROBE]
        while pending:
            source = pending.pop()
            for name in re.findall(r'^\s*#include\s+"([^"]+)"', source.read_text(encoding="utf-8"), re.MULTILINE):
                header = (source.parent / name).resolve()
                if header not in paths:
                    header.relative_to(ROOT)
                    paths.add(header)
                    pending.append(header)
        return sorted(paths)
    # 逐文件白名单：生成目录、测试 SDK 和历史构建不能成为源码快照输入。
    paths.update(ROOT / "kexts" / name for name in BUNDLE_INPUTS)
    return sorted(paths)


def source_changes(manifest, reuse=False):
    current = {p.relative_to(ROOT).as_posix(): p for p in source_paths(reuse)}
    changed = sorted(set(current) ^ set(manifest))
    for name in sorted(set(current) & set(manifest)):
        if hashlib.sha256(current[name].read_bytes()).hexdigest() != manifest[name]:
            changed.append(name)
    return sorted(changed)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--build-bundle", action="store_true", help="从本次隔离源码调用已有 build.sh")
    group.add_argument("--bundle-executable", help="显式给定远端已构建 bundle 内可执行映像绝对路径")
    parser.add_argument("--bundle-sha256", help="复用映像时必须给定预期 SHA256")
    args = parser.parse_args()
    if args.bundle_executable:
        if not PurePosixPath(args.bundle_executable).is_absolute() or "\n" in args.bundle_executable:
            parser.error("--bundle-executable 必须为远端绝对路径")
        if not args.bundle_sha256 or not re.fullmatch(r"[0-9a-fA-F]{64}", args.bundle_sha256):
            parser.error("复用 bundle 必须提供 --bundle-sha256；不猜测路径或构建身份")
    elif args.bundle_sha256:
        parser.error("--bundle-sha256 仅用于 --bundle-executable")
    return args


def main():
    args = arguments()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    local = ROOT / "out" / ("integration-render-probe-" + stamp)
    if not local.parent.is_dir():
        raise RuntimeError("out 父目录不存在")
    local.mkdir()
    remote = "/var/tmp/vgpu-integration-render-probe-" + stamp
    manifest, binary_hashes = {}, {}
    status, client, bundle = 1, None, None
    q = shlex.quote
    with (local / "build.log").open("w", encoding="utf-8", buffering=1) as log:
        def record(text):
            print(text, flush=True)
            log.write(text + "\n")

        def unchanged():
            changed = source_changes(manifest, bool(args.bundle_executable))
            record("WORKSPACE_CHANGED " + json.dumps(changed))
            if changed:
                raise RuntimeError("源码已变化：拒绝交付，需要从新快照重建")

        def run(command):
            record("COMMAND " + command)
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
                rc = mssh.run(client, command, 240)
            record(capture.getvalue())
            record("EXIT " + str(rc))
            if rc:
                raise RuntimeError("远端验证失败；停止并保留日志")

        def verify_remote_sources():
            command = "cd " + q(remote) + " && shasum -a 256 " + " ".join(q(name) for name in manifest)
            stdin, stdout, stderr = client.exec_command(command, timeout=120)
            stdin.close()
            text = stdout.read().decode("utf-8", "strict")
            error = stderr.read()
            if stdout.channel.recv_exit_status() or error:
                raise RuntimeError("远端源码哈希检查失败")
            observed = {}
            for line in text.splitlines():
                digest, relative = line.split(None, 1)
                observed[relative] = digest
            if observed != manifest:
                raise RuntimeError("远端源码哈希与快照不匹配")
            record("REMOTE_SOURCE_HASHES_PASS " + str(len(manifest)))

        def remote_hash(path):
            stdin, stdout, stderr = client.exec_command("shasum -a 256 " + q(path), timeout=120)
            stdin.close()
            text = stdout.read().decode("ascii", "strict")
            error = stderr.read()
            if stdout.channel.recv_exit_status() or error or not re.match(r"^[0-9a-f]{64}  ", text):
                raise RuntimeError("远端文件哈希检查失败")
            return text[:64]

        try:
            with tarfile.open(local / "source.tar", "w") as archive:
                for path in source_paths(bool(args.bundle_executable)):
                    relative = path.relative_to(ROOT).as_posix()
                    data = path.read_bytes()
                    manifest[relative] = hashlib.sha256(data).hexdigest()
                    entry = tarfile.TarInfo(relative)
                    entry.size = len(data)
                    entry.mode = 0o644
                    archive.addfile(entry, io.BytesIO(data))
            (local / "source-sha256.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            unchanged()
            # SSH 模块也来自记录的字节，不从可能被并发修改的工作区重新导入。
            with tarfile.open(local / "source.tar") as archive:
                ssh_source = archive.extractfile("tools/mssh.py").read()
            spec = importlib.util.spec_from_loader("integration_probe_mssh", loader=None)
            mssh = importlib.util.module_from_spec(spec)
            mssh.__file__ = str(ROOT / "tools/mssh.py")
            exec(compile(ssh_source, mssh.__file__, "exec"), mssh.__dict__)
            mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
            client = mssh.connect(retries=1)
            record("LOCAL " + str(local) + "\nREMOTE " + remote)
            run("sw_vers && uname -m && xcrun --show-sdk-path && xcrun --show-sdk-version && xcrun clang --version")
            run("test -d /var/tmp && mkdir " + q(remote))
            with client.open_sftp() as sftp:
                sftp.put(str(local / "source.tar"), remote + "/source.tar")
            archive_hash = hashlib.sha256((local / "source.tar").read_bytes()).hexdigest()
            if remote_hash(remote + "/source.tar") != archive_hash:
                raise RuntimeError("上传快照归档哈希不匹配")
            run("tar -xf " + q(remote + "/source.tar") + " -C " + q(remote))
            verify_remote_sources()
            unchanged()
            if args.build_bundle:
                run("bash " + q(remote + "/kexts/gld-integration/build.sh") + " " + q(remote + "/build"))
                bundle = remote + "/build/VGPUDriverIntegration.bundle/Contents/MacOS/VGPUDriverIntegration"
                binary_hashes["bundle"] = remote_hash(bundle)
            else:
                # 复制确切映像到本次快照，检查复制前后身份；不在共享构建目录写文件。
                run("test -f " + q(args.bundle_executable))
                expected = args.bundle_sha256.lower()
                if remote_hash(args.bundle_executable) != expected:
                    raise RuntimeError("复用 bundle 的 SHA256 与参数不一致")
                bundle = remote + "/VGPUDriverIntegration"
                run("cp " + q(args.bundle_executable) + " " + q(bundle))
                if remote_hash(bundle) != expected or remote_hash(args.bundle_executable) != expected:
                    raise RuntimeError("复用 bundle 在隔离复制时改变")
                binary_hashes["bundle"] = expected
                record("REUSED_BUNDLE：仅确认映像哈希；不声称与当前源码一一对应")
            flags = ("-arch x86_64 -mmacosx-version-min=15.0 -std=c11 -O1 -g "
                     "-Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow "
                     "-Wstrict-prototypes -Wmissing-prototypes")
            links = "-framework IOKit -framework CoreFoundation"
            for name, extra in (("gld_integration_render_probe", ""),
                                ("gld_integration_render_probe_sanitized",
                                 "-fsanitize=address,undefined -fno-omit-frame-pointer")):
                binary = remote + "/" + name
                # 只编译探针一个翻译单元：fixture、integration 核心和 surface 均不链接。
                run("xcrun clang " + flags + " " + extra + " " + q(remote + "/" + PROBE) +
                    " " + links + " -o " + q(binary))
                environment = "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "
                run(environment + q(binary) + " --cpu-only")
                run(environment + q(binary) + " --empty --bundle " + q(bundle))
                for invalid in ("", "--invalid", "--gpu --cpu-only", "--empty --bundle relative"):
                    run(q(binary) + " " + invalid + "; rc=$?; test \"$rc\" -eq 2")
                run("otool -L " + q(binary) + " && xcrun nm -u " + q(binary))
                binary_hashes[name] = remote_hash(binary)
                with client.open_sftp() as sftp:
                    sftp.get(binary, str(local / name))
                if hashlib.sha256((local / name).read_bytes()).hexdigest() != binary_hashes[name]:
                    raise RuntimeError("下载二进制哈希不匹配")
            # 已有回归单独链接 CPU 模型，绝不混入 dlopen 探针或 bundle。
            cpu_sources = (
                "gld-integration/integration.c", "gld-integration/context_exports.c",
                "gld-integration/render_exports.c", "gld-context/vgpu_context.c",
                "gld-objects/vgpu_objects.c", "gld-texture/vgpu_texture.c",
                "gld-texture/vgpu_texture_bgra.c", "gld-framebuffer/vgpu_framebuffer.c",
                "gld-state/vgpu_clear_state.c", "gld-state/vgpu_triangle_state.c",
                "gld-integration/tcl_exports.c", "gld-integration/dispatch_exports.c",
                "gld-pixel/gld_pixel.c", "gld-integration/integration_read_test.c",
            )
            inputs = " ".join(q(remote + "/kexts/" + name) for name in cpu_sources)
            cpu_modes = (("strict", ""), ("sanitized", "-fsanitize=address,undefined -fno-omit-frame-pointer")) if args.build_bundle else ()
            for mode, extra in cpu_modes:
                binary = remote + "/integration_read_cpu_" + mode
                run("xcrun clang " + flags + " " + extra + " " + inputs + " -o " + q(binary))
                run("ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 " + q(binary))
                binary_hashes["integration_read_cpu_" + mode] = remote_hash(binary)
            if remote_hash(bundle) != binary_hashes["bundle"]:
                raise RuntimeError("bundle 在 CPU 验证期间改变")
            verify_remote_sources()
            unchanged()
            record("BINARY_SHA256 " + json.dumps(binary_hashes, sort_keys=True))
            record("GPU 验收未执行；系统 texture 视图及 FBO load/clear/read 结果须由主代理实际测试确定，BLOCKED 不算通过。")
            record("主代理串行执行命令（本 runner 未运行 GPU）：")
            record("python tools/mssh.py --sudo -- " + q(remote + "/gld_integration_render_probe") +
                   " --gpu --bundle " + q(bundle))
            status = 0
        except (Exception, SystemExit) as exc:
            # 不输出连接异常内容，避免凭据/连接参数进入结果。
            record("FAILED " + type(exc).__name__)
            if isinstance(exc, RuntimeError):
                record(str(exc))
        finally:
            (local / "result.json").write_text(json.dumps({
                "status": status, "remote": remote, "bundle": bundle,
                "manifest": manifest, "binary_sha256": binary_hashes,
                "gpu_executed": False, "fixture_linked_into_probe": False,
                "gpu_acceptance": "未执行；由实际 GPU 测试确定，BLOCKED 不算通过",
                "reused_bundle": args.bundle_executable,
            }, indent=2, ensure_ascii=False), encoding="utf-8")
            record("FINAL " + str(status) + " " + str(local))
            if client:
                client.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
