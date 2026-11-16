"""隔离快照、真实 macOS SDK 严格/ASan+UBSan 构建与 CPU 自检；永不执行 GPU。"""
from datetime import datetime, timezone
from pathlib import Path
import contextlib
import hashlib
import io
import json
import shlex
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import mssh


def main():
    if len(sys.argv) != 1:
        print("用法：build_primitive_draw_probe.py（仅编译和 CPU 自检）", file=sys.stderr)
        return 2
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    local = ROOT / "out" / ("primitive-draw-macos-" + stamp)
    if not local.parent.is_dir():
        raise RuntimeError("out 父目录不存在")
    local.mkdir()
    remote = "/var/tmp/vgpu-primitive-draw-" + stamp
    files = (
        "gld-context/vgpu_context.c", "gld-context/vgpu_context.h",
        "gld-surface/vgpu_surface.c", "gld-surface/vgpu_surface.h",
        "gld-integration/primitive_exports.c", "gld-integration/primitive_exports.h",
        "probes/gld_resource_probe.c", "probes/gld_primitive_draw_probe.c",
        "probes/build_primitive_draw_probe.py",
    )
    manifest = {}
    binary_hashes = {}
    status = 1
    client = None
    with (local / "build.log").open("w", encoding="utf-8", buffering=1) as log:
        def record(text):
            print(text, flush=True)
            log.write(text + "\n")

        def run(command):
            record("COMMAND " + command)
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
                rc = mssh.run(client, command, 180)
            record(capture.getvalue())
            record("EXIT " + str(rc))
            if rc:
                raise RuntimeError("远端命令失败，保留现场并停止")

        try:
            for relative in files:
                data = (ROOT / "kexts" / relative).read_bytes()
                target = local / relative
                target.parent.mkdir(exist_ok=True)
                target.write_bytes(data)
                manifest[relative] = hashlib.sha256(data).hexdigest()
            (local / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            record("LOCAL " + str(local) + "\nREMOTE " + remote)
            mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
            client = mssh.connect(retries=1)
            run("sw_vers && xcrun --show-sdk-path && xcrun clang --version")
            run("test -d /var/tmp && mkdir " + shlex.quote(remote))
            run("mkdir " + " ".join(shlex.quote(remote + "/" + directory)
                                     for directory in ("gld-context", "gld-surface", "gld-integration", "probes")))
            with client.open_sftp() as sftp:
                for relative in files:
                    sftp.put(str(local / relative), remote + "/" + relative)
                    with sftp.open(remote + "/" + relative, "rb") as stream:
                        if hashlib.sha256(stream.read()).hexdigest() != manifest[relative]:
                            raise RuntimeError("上传哈希不匹配：" + relative)
            sources = ("gld-context/vgpu_context.c gld-surface/vgpu_surface.c "
                       "gld-integration/primitive_exports.c probes/gld_primitive_draw_probe.c")
            flags = "-std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic"
            links = "-framework IOKit -framework CoreFoundation -lpthread"
            prefix = "cd " + shlex.quote(remote) + " && "
            run(prefix + f"xcrun clang {flags} {sources} {links} -o gld_primitive_draw_probe")
            run(prefix + f"xcrun clang {flags} -fsanitize=address,undefined -fno-omit-frame-pointer "
                         f"{sources} {links} -o gld_primitive_draw_probe_sanitized")
            run(prefix + "./gld_primitive_draw_probe --oracle-only")
            run(prefix + "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "
                         "./gld_primitive_draw_probe_sanitized --oracle-only")
            run(prefix + "./gld_primitive_draw_probe --oracle-colored")
            run(prefix + "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "
                         "./gld_primitive_draw_probe_sanitized --oracle-colored")
            # 参数错误必须退出 2，不允许默认进入 GPU 路径。
            for arguments in ("", "--invalid", "--gpu --oracle-only"):
                run(prefix + "./gld_primitive_draw_probe " + arguments +
                    "; result=$?; test \"$result\" -eq 2")
            run(prefix + "shasum -a 256 " + " ".join(files) +
                " gld_primitive_draw_probe gld_primitive_draw_probe_sanitized")
            with client.open_sftp() as sftp:
                for name in ("gld_primitive_draw_probe", "gld_primitive_draw_probe_sanitized"):
                    sftp.get(remote + "/" + name, str(local / name))
                    data = (local / name).read_bytes()
                    binary_hashes[name] = hashlib.sha256(data).hexdigest()
                    with sftp.open(remote + "/" + name, "rb") as stream:
                        if hashlib.sha256(stream.read()).hexdigest() != binary_hashes[name]:
                            raise RuntimeError("下载二进制哈希不匹配：" + name)
            changed = [relative for relative in files
                       if hashlib.sha256((ROOT / "kexts" / relative).read_bytes()).hexdigest()
                       != manifest[relative]]
            record("WORKSPACE_CHANGED " + json.dumps(changed))
            if changed:
                raise RuntimeError("构建期间源文件改变；仅记录隔离快照，需重新构建")
            record("待主代理串行 GPU 执行（本 runner 未执行）：")
            record("python tools/mssh.py --sudo -- " + remote + "/gld_primitive_draw_probe --gpu")
            status = 0
        except (Exception, SystemExit) as exc:
            # 不记录 SSH 异常细节，避免连接参数或凭据出现在交付日志。
            record("FAILED " + type(exc).__name__)
        finally:
            (local / "result.json").write_text(json.dumps({
                "status": status, "remote": remote, "manifest": manifest,
                "binary_sha256": binary_hashes, "gpu_executed": False,
            }, indent=2), encoding="utf-8")
            record("FINAL " + str(status) + " " + str(local))
            if client:
                client.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
