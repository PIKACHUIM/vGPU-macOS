"""隔离构建并在已授权客体运行真实 GPU probe；旧失败日志原样保留。"""
import contextlib
import hashlib
import io
import pathlib
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import mssh  # noqa: E402

REMOTE = "/var/tmp/vgpu-context-independent"


def main():
    mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
    client = mssh.connect()
    log = [f"RUN UTC {datetime.now(timezone.utc).isoformat()}\n"]
    status = 1
    completed = []
    analyzer_warnings = []
    try:
        def run(command, sudo=False):
            output = io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                code = mssh.run(client, command, 180, use_sudo=sudo,
                                password=mssh.os.environ.get("VG_PASS"))
            text = output.getvalue()
            log.append(f"$ sudo={sudo} {command}\n{text}\nexit={code}\n")
            print(text, end="", flush=True)
            return code

        def require(command, sudo=False):
            code = run(command, sudo)
            if code:
                raise RuntimeError(f"客体命令退出 {code}")

        def diagnose():
            run("id; sysctl kern.boottime; ioreg -r -c vgpuAccelerator -l; "
                "ps -axo pid,ppid,user,lstart,comm")

        diagnose()
        require(f"test -d /var/tmp && mkdir -p {REMOTE}/gld-context {REMOTE}/gld-surface {REMOTE}/probes")
        sftp = client.open_sftp()
        try:
            for rel in ("gld-context/vgpu_context.h", "gld-context/vgpu_context.c",
                        "gld-surface/vgpu_surface.h", "gld-surface/vgpu_surface.c",
                        "probes/gld_context_probe.c"):
                source = ROOT / "kexts" / rel
                log.append(f"SHA256 {hashlib.sha256(source.read_bytes()).hexdigest()} {rel}\n")
                sftp.put(str(source), f"{REMOTE}/{rel}")
        finally:
            sftp.close()
        log.append(f"SHA256 {hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()} verify.py\n")
        require("clang --version; sw_vers")
        flags = "-std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow -g -O1"
        sources = "gld-context/vgpu_context.c gld-surface/vgpu_surface.c probes/gld_context_probe.c"
        links = "-framework IOKit -framework CoreFoundation -lpthread"
        require(f"cd {REMOTE} && clang {flags} {sources} {links} -o gld_context_probe")
        completed.append("普通严格构建")
        require(f"cd {REMOTE} && clang {flags} -fsanitize=address,undefined "
                f"-fno-omit-frame-pointer {sources} {links} -o gld_context_probe_sanitized")
        completed.append("ASan/UBSan 严格构建")
        for source in sources.split():
            require(f"cd {REMOTE} && clang --analyze {flags} -Xanalyzer -analyzer-output=text {source}")
            analyzer_warnings.extend(line for line in log[-1].splitlines() if "warning:" in line)
        completed.append(f"clang --analyze 三个源文件（{len(analyzer_warnings)} 条告警）")
        require(f"cd {REMOTE} && shasum -a 256 {sources} gld_context_probe gld_context_probe_sanitized")
        require(f"{REMOTE}/gld_context_probe --expect-denied")
        completed.append("非 root NotPrivileged（3 项断言）")
        # 没有自动重试：意外 NotReady 时采集 boot、服务和并发信息，保留原始失败。
        for executable in ("gld_context_probe", "gld_context_probe_sanitized"):
            require("if pgrep -x gld_surface_probe || pgrep -x gld_context_probe || "
                    "pgrep -x gld_context_probe_sanitized || pgrep -x surface-probe || "
                    "pgrep -x surface-probe-sanitized; then exit 76; fi")
            require(f"id && ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "
                    f"UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 {REMOTE}/{executable}", True)
            completed.append(f"root {executable}")
        require("sysctl kern.boottime; ioreg -r -c vgpuAccelerator -l")
        status = 0
    except Exception as exc:
        log.append(f"失败：{type(exc).__name__}: {exc}\n")
        diagnose()
    finally:
        raw = "\n".join(log)
        with (HERE / "gpu-raw.log").open("a", encoding="utf-8") as output:
            output.write("\n===== 权限修正后的独立验证；以上历史原始日志保持不变 =====\n" + raw)
        evidence = "\n".join(line for line in raw.splitlines()
                             if line.startswith(("RESULT ", "PIXELS ", "GENERATION ", "SHA256 ", "GPU clear/read")))
        summary = (
            "更正：历史 0xe00002c1 为 kIOReturnNotPrivileged，来自非 root share open；"
            "不是服务不存在。kIOReturnNotFound 为 0xe00002f0。历史失败原始日志保留在 gpu-raw.log 开头。\n"
            f"本次状态：{'PASS' if status == 0 else 'FAIL'}；完成：{'、'.join(completed)}。\n"
            "sudo 密码仅经 stdin；没有安装、重启或自动 NotReady 重试。"
            "LeakSanitizer 在该客体不受支持，关闭 detect_leaks，ASan/UBSan 保持启用。\n"
            + "\n".join(analyzer_warnings) + "\n"
            + "审查：context→draw/read framebuffer→image→share 引用有序释放；同一 draw/read 各持引用；"
            "image 析构释放 lease；load 失败保持 dirty，clear 失败保持 dirty/failed 与旧 generation。\n"
            + evidence + "\n完整命令、源文件与可执行文件 SHA256、boot/服务/进程证据见 gpu-raw.log。\n"
        )
        (HERE / "results.txt").write_text(summary, encoding="utf-8")
        client.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
