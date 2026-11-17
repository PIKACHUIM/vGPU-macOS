"""受控绘制探针隔离构建：仅 CPU oracle/空表验证，绝不执行 GPU、安装或提权。"""
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
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
PROBE = "kexts/probes/gld_controlled_draw_probe.c"
RUNNER = "kexts/probes/build_controlled_draw_probe.py"


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-executable", required=True, help="明确的远端 bundle executable 绝对路径")
    parser.add_argument("--bundle-sha256", required=True, help="该远端映像的预期 SHA256")
    args = parser.parse_args()
    if not PurePosixPath(args.bundle_executable).is_absolute() or any(
            ord(c) < 32 or ord(c) == 127 for c in args.bundle_executable):
        parser.error("--bundle-executable 必须为无控制字符的远端绝对路径")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.bundle_sha256):
        parser.error("--bundle-sha256 必须为64位十六进制摘要")
    args.bundle_sha256 = args.bundle_sha256.lower()
    return args


def snapshot():
    # 只收集单个探针和递归本地头文件，不打包或链接其他探针/fixture/驱动实现。
    paths = [ROOT / PROBE, ROOT / RUNNER, ROOT / "tools/mssh.py"]
    data = {}
    while paths:
        path = paths.pop().resolve()
        name = path.relative_to(ROOT).as_posix()
        if name in data:
            continue
        raw = path.read_bytes()
        data[name] = raw
        if path.suffix in (".c", ".h"):
            for include in re.findall(r'^\s*#include\s+"([^"]+)"', raw.decode("utf-8"), re.MULTILINE):
                header = (path.parent / include).resolve()
                if header.suffix != ".h":
                    raise RuntimeError("探针仅允许包含头文件")
                paths.append(header)
    return data


def main():
    args = arguments()
    identity = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + uuid.uuid4().hex[:8]
    local = ROOT / "out" / ("controlled-draw-probe-" + identity)
    if not local.parent.is_dir():
        raise RuntimeError("out 父目录不存在")
    local.mkdir()
    remote = "/var/tmp/vgpu-controlled-draw-probe-" + identity
    bundle = remote + "/VGPUDriverIntegration"
    result, client, manifest, hashes = 1, None, {}, {}
    q = shlex.quote
    with (local / "build.log").open("w", encoding="utf-8", buffering=1) as log:
        def record(text):
            print(text, flush=True)
            log.write(text + "\n")

        def run(command):
            record("COMMAND " + command)
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
                rc = mssh.run(client, command, 240)
            record(capture.getvalue())
            record("EXIT " + str(rc))
            if rc:
                raise RuntimeError("远端构建或 CPU 验证失败；停止交付")

        def remote_hash(path):
            stdin, stdout, stderr = client.exec_command("shasum -a 256 " + q(path), timeout=120)
            stdin.close()
            output, error = stdout.read().decode("ascii", "strict"), stderr.read()
            if stdout.channel.recv_exit_status() or error or not re.match(r"^[0-9a-f]{64}  ", output):
                raise RuntimeError("远端哈希校验失败")
            return output[:64]

        def unchanged():
            current = {name: hashlib.sha256(raw).hexdigest() for name, raw in snapshot().items()}
            if current != manifest:
                raise RuntimeError("本地依赖已改变，须使用新快照重新验证")
            for name, digest in manifest.items():
                if remote_hash(remote + "/" + name) != digest:
                    raise RuntimeError("远端源码与固定快照不一致")
            if remote_hash(bundle) != args.bundle_sha256:
                raise RuntimeError("快照 bundle 已改变")
            record("SOURCE_AND_BUNDLE_HASHES_PASS")

        try:
            sources = snapshot()
            manifest = {name: hashlib.sha256(raw).hexdigest() for name, raw in sources.items()}
            archive_path = local / "source.tar"
            with tarfile.open(archive_path, "w") as archive:
                for name, raw in sorted(sources.items()):
                    entry = tarfile.TarInfo(name)
                    entry.size, entry.mode = len(raw), 0o644
                    archive.addfile(entry, io.BytesIO(raw))
            (local / "source-sha256.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            spec = importlib.util.spec_from_loader("controlled_draw_mssh", loader=None)
            mssh = importlib.util.module_from_spec(spec)
            mssh.__file__ = str(ROOT / "tools/mssh.py")
            exec(compile(sources["tools/mssh.py"], mssh.__file__, "exec"), mssh.__dict__)
            mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
            client = mssh.connect(retries=1)
            record("LOCAL " + str(local) + "\nREMOTE " + remote)
            run("sw_vers && uname -m && xcrun --show-sdk-version && xcrun clang --version")
            run("test -d /var/tmp && mkdir " + q(remote))
            with client.open_sftp() as sftp:
                sftp.put(str(archive_path), remote + "/source.tar")
            if remote_hash(remote + "/source.tar") != hashlib.sha256(archive_path.read_bytes()).hexdigest():
                raise RuntimeError("上传归档哈希不一致")
            run("tar -xf " + q(remote + "/source.tar") + " -C " + q(remote))
            if remote_hash(args.bundle_executable) != args.bundle_sha256:
                raise RuntimeError("指定 bundle SHA256 不匹配")
            run("cp " + q(args.bundle_executable) + " " + q(bundle))
            if remote_hash(bundle) != args.bundle_sha256 or remote_hash(args.bundle_executable) != args.bundle_sha256:
                raise RuntimeError("bundle 复制前后身份改变")
            hashes["bundle"] = args.bundle_sha256
            unchanged()
            flags = ("-arch x86_64 -mmacosx-version-min=15.0 -std=c11 -O1 -g "
                     "-Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow "
                     "-Wstrict-prototypes -Wmissing-prototypes")
            environment = "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "
            for suffix, extra in (("", ""), ("_sanitized", "-fsanitize=address,undefined -fno-omit-frame-pointer")):
                name = "gld_controlled_draw_probe" + suffix
                binary = remote + "/" + name
                run("xcrun clang " + flags + " " + extra + " " + q(remote + "/" + PROBE) +
                    " -framework IOKit -framework CoreFoundation -o " + q(binary))
                run(environment + q(binary) + " --cpu-only")
                run(environment + q(binary) + " --empty --bundle " + q(bundle))
                # 无有效 --gpu 命令；参数负例均在 dlopen 前结束。
                for invalid in ("", "--invalid", "--empty --bundle relative"):
                    run(q(binary) + " " + invalid + '; rc=$?; test "$rc" -eq 2')
                run("otool -L " + q(binary) + " && xcrun nm -u " + q(binary))
                hashes[name] = remote_hash(binary)
                with client.open_sftp() as sftp:
                    sftp.get(binary, str(local / name))
                if hashlib.sha256((local / name).read_bytes()).hexdigest() != hashes[name]:
                    raise RuntimeError("下载探针哈希不一致")
            unchanged()
            record("BINARY_SHA256 " + json.dumps(hashes, sort_keys=True))
            record("PASS BUILD_CPU_ONLY；ASan/UBSan只覆盖探针，复用bundle未宣称已插桩；GPU未执行")
            record("主代理独占串行 GPU 命令（本 runner 不执行）：")
            record("python tools/mssh.py --sudo -- " + q(remote + "/gld_controlled_draw_probe") +
                   " --gpu --bundle " + q(bundle))
            result = 0
        except (Exception, SystemExit) as exc:
            record("BLOCKED " + type(exc).__name__)
            if isinstance(exc, RuntimeError):
                record(str(exc))
        finally:
            (local / "result.json").write_text(json.dumps({
                "status": result, "remote": remote, "bundle": bundle,
                "source_sha256": manifest, "binary_sha256": hashes,
                "gpu_executed": False, "gpu_acceptance": "NOT_RUN；缺前提必须BLOCKED非PASS",
                "fixture_linked": False, "bundle_instrumented": "未作声明",
                "original_bundle": args.bundle_executable,
            }, indent=2, ensure_ascii=False), encoding="utf-8")
            record("FINAL " + str(result) + " " + str(local))
            if client:
                client.close()
    return result


if __name__ == "__main__":
    sys.exit(main())
