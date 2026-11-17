"""仅隔离真实 macOS SDK 编译和纯 CPU oracle；不执行公开 CGL 二进制。

本地仅读取指定两个源文件；构建产物留在唯一远端 /var/tmp 目录。
SSH 凭据由 tools/mssh.py 读取，异常不输出连接参数或凭据。
"""
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
        print("用法：build_public_triangle_probe.py（仅编译和 CPU 自检）", file=sys.stderr)
        return 2
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    remote = "/var/tmp/vgpu-public-triangle-" + stamp
    names = ("gl_triangle_cgl_probe.c", "build_public_triangle_probe.py")
    snapshot = {name: (ROOT / "kexts/probes" / name).read_bytes() for name in names}
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in snapshot.items()}
    binaries = {}
    client = None
    status = 1

    def record(text):
        print(text, flush=True)

    def run(command):
        record("COMMAND " + command)
        capture = io.StringIO()
        with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
            rc = mssh.run(client, command, 180)
        record(capture.getvalue())
        record("EXIT " + str(rc))
        if rc:
            raise RuntimeError("远端命令失败；保留现场并停止")

    try:
        record("REMOTE " + remote)
        record("SOURCE_SHA256 " + json.dumps(manifest, sort_keys=True))
        mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
        client = mssh.connect(retries=1)
        run("sw_vers && xcrun --sdk macosx --show-sdk-path && xcrun --sdk macosx --show-sdk-version && xcrun clang --version")
        _, sdk_stdout, _ = client.exec_command("xcrun --sdk macosx --show-sdk-path")
        sdk = sdk_stdout.read().decode().strip()
        if sdk_stdout.channel.recv_exit_status() != 0 or not sdk.startswith("/"):
            raise RuntimeError("无法读取 SDK 路径")
        with client.open_sftp() as sftp:
            for header in ("OpenGL.h", "CGLTypes.h"):
                path = sdk + "/System/Library/Frameworks/OpenGL.framework/Headers/" + header
                with sftp.open(path, "rb") as stream:
                    data = stream.read()
                record("SDK_HEADER_SHA256 " + header + " " + hashlib.sha256(data).hexdigest())
                for line in data.decode().splitlines():
                    if any(name in line for name in ("CGLCreatePBuffer", "CGLSetPBuffer",
                            "CGLGetPBuffer", "CGLDescribePBuffer", "CGLDestroyPBuffer",
                            "CGLClearDrawable", "kCGLPFAPBuffer")):
                        record("SDK_DECL " + line.strip())
        run("test -d /var/tmp && mkdir " + shlex.quote(remote))
        with client.open_sftp() as sftp:
            for name, data in snapshot.items():
                with sftp.open(remote + "/" + name, "wb") as stream:
                    stream.write(data)
                with sftp.open(remote + "/" + name, "rb") as stream:
                    if hashlib.sha256(stream.read()).hexdigest() != manifest[name]:
                        raise RuntimeError("上传哈希不匹配")
        prefix = "cd " + shlex.quote(remote) + " && "
        compiler = 'xcrun --sdk macosx clang -isysroot "$(xcrun --sdk macosx --show-sdk-path)" '
        flags = "-std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic "
        source = "gl_triangle_cgl_probe.c "
        run(prefix + compiler + flags + source + "-framework OpenGL -o gl_triangle_cgl_probe")
        run(prefix + compiler + flags + "-DTRIANGLE_ORACLE_ONLY " + source + "-o triangle_oracle")
        run(prefix + compiler + flags + "-DTRIANGLE_ORACLE_ONLY "
            "-fsanitize=address,undefined -fno-omit-frame-pointer " + source + "-o triangle_oracle_sanitized")
        # 用工具读取链接信息，不加载/运行公开 CGL 二进制。
        run(prefix + "otool -L gl_triangle_cgl_probe && nm -u gl_triangle_cgl_probe")
        run(prefix + "otool -L triangle_oracle && nm -u triangle_oracle")
        run(prefix + "./triangle_oracle --oracle-only")
        run(prefix + "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "
            "./triangle_oracle_sanitized --oracle-only")
        for args in ("", "--choose-only", "--context-only", "--clear-read", "--gpu", "--reject-color",
                     "--pbuffer-choose-only", "--pbuffer-context-only", "--pbuffer-clear-read",
                     "--invalid", "--oracle-only --gpu", "--oracle-only --pbuffer-clear-read"):
            run(prefix + "./triangle_oracle " + args + '; result=$?; test "$result" -eq 2')
        artifacts = ("gl_triangle_cgl_probe", "triangle_oracle", "triangle_oracle_sanitized")
        run(prefix + "shasum -a 256 " + " ".join(names + artifacts))
        with client.open_sftp() as sftp:
            for name in artifacts:
                with sftp.open(remote + "/" + name, "rb") as stream:
                    binaries[name] = hashlib.sha256(stream.read()).hexdigest()
        changed = [name for name in names
                   if hashlib.sha256((ROOT / "kexts/probes" / name).read_bytes()).hexdigest() != manifest[name]]
        record("WORKSPACE_CHANGED " + json.dumps(changed))
        if changed:
            raise RuntimeError("源文件在编译期间变动，需重新隔离构建")
        record("PUBLIC_CGL_NOT_EXECUTED：公开验收尚未运行；编译/CPU 通过不是 GPU 通过。")
        record("PBUFFER_NOT_RUN：需另行确认自有 renderer 的 PBuffer 能力、隔离恢复和 runner 支持后逐阶段串行验收。")
        for stage in ("pbuffer-choose-only", "pbuffer-context-only", "pbuffer-clear-read"):
            record("PBuffer 阶段（本脚本未执行）：" + remote + "/gl_triangle_cgl_probe --" + stage)
        record("格式选择（本脚本未执行）：" + remote + "/gl_triangle_cgl_probe --choose-only")
        record("初始化阶段（本脚本未执行）：" + remote + "/gl_triangle_cgl_probe --context-only")
        record("清屏读回阶段（本脚本未执行）：" + remote + "/gl_triangle_cgl_probe --clear-read")
        record("待主代理串行运行（本脚本未执行）：" + remote + "/gl_triangle_cgl_probe --gpu")
        record("独立受限颜色拒绝（本脚本未执行）：" + remote + "/gl_triangle_cgl_probe --reject-color")
        status = 0
    except (Exception, SystemExit) as exc:
        record("FAILED " + type(exc).__name__)
    finally:
        if client:
            client.close()
        record("RESULT " + json.dumps({
            "status": status, "remote": remote, "source_sha256": manifest,
            "binary_sha256": binaries, "gpu_executed": False, "cgl_executed": False,
            "public_acceptance": "NOT_RUN",
        }, sort_keys=True))
    return status


if __name__ == "__main__":
    sys.exit(main())
