"""独立 triangle state CPU 验证；--macos-ssh 在新临时目录使用真实SDK，不访问GPU。"""
from datetime import datetime, timezone
import argparse
import contextlib
import hashlib
import io
import shlex
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def verify_macos(files, hashes, log, stamp):
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ROOT / "tools"))
    import mssh

    remote = "/var/tmp/vgpu-triangle-state-cpu-" + stamp
    mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
    client = mssh.connect(retries=1)

    def run(command):
        log.append("REMOTE_COMMAND " + command)
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            status = mssh.run(client, command, 120)
        text = output.getvalue()
        log.extend([text, f"REMOTE_EXIT {status}"])
        print(text, end="", flush=True)
        if status:
            raise RuntimeError(f"远端CPU命令失败，退出码{status}")
        return text

    try:
        run("sw_vers && uname -m && xcrun --find clang && xcrun --show-sdk-path && xcrun clang --version")
        run(f"test -d /var/tmp && mkdir {remote} && mkdir {remote}/kexts && "
            f"mkdir {remote}/kexts/gld-state {remote}/kexts/gld-context {remote}/kexts/gld-surface")
        # 只上传自有源码及依赖头，不上传 IOKit fixture，强制使用真实SDK。
        uploaded = [path for path in files if "cpu-fixture" not in path.parts]
        sftp = client.open_sftp()
        try:
            for path in uploaded:
                target = remote + "/" + path.relative_to(ROOT).as_posix()
                sftp.put(str(path), target)
        finally:
            sftp.close()
        paths = " ".join(shlex.quote(p.relative_to(ROOT).as_posix()) for p in uploaded)

        def check_hashes():
            text = run(f"cd {remote} && shasum -a 256 {paths}")
            actual = {line.split(None, 1)[1].strip(): line.split(None, 1)[0]
                      for line in text.splitlines() if line.strip()}
            expected = {p.relative_to(ROOT).as_posix(): hashes[p] for p in uploaded}
            if actual != expected:
                raise RuntimeError("远端源码 SHA256 与本地测试来源不一致")

        check_hashes()
        flags = ('-isysroot "$(xcrun --show-sdk-path)" -std=c11 -Wall -Wextra '
                 '-Werror -Wpedantic -Wconversion -Wshadow -O1 -g')
        inputs = "kexts/gld-state/vgpu_triangle_state.c kexts/gld-state/triangle_state_cpu_probe.c"
        for mode, extra in [("strict", ""), ("ndebug", "-DNDEBUG"),
                            ("ubsan", "-fsanitize=undefined -fno-sanitize-recover=all")]:
            run(f"cd {remote} && xcrun clang {flags} {extra} {inputs} -o {mode}")
            run(f"cd {remote} && shasum -a 256 {mode} && ./{mode}")
            log.append("REMOTE_MODE PASS " + mode)
        check_hashes()
        log.append("REMOTE_RESULT PASS " + remote)
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--macos-ssh", action="store_true", help="额外执行隔离的真实macOS SDK CPU测试")
    args = parser.parse_args()
    zig = ROOT / "out/.ziglang/ziglang/zig.exe"
    fixture = ROOT / "kexts/gld-objects/cpu-fixture"
    if not zig.is_file() or not (ROOT / "out").is_dir() or not fixture.is_dir():
        raise RuntimeError("指定编译器、out或既有IOKit fixture不存在")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = ROOT / "out" / ("triangle-state-cpu-" + stamp)
    out.mkdir()
    source = HERE / "vgpu_triangle_state.c"
    probe = HERE / "triangle_state_cpu_probe.c"
    files = [source, HERE / "vgpu_triangle_state.h", probe, Path(__file__).resolve(),
             ROOT / "kexts/gld-context/vgpu_context.h",
             ROOT / "kexts/gld-surface/vgpu_surface.h", fixture / "IOKit/IOReturn.h"]
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion",
             "-Wshadow", "-O1", "-g", "-I" + str(fixture)]
    if sys.platform == "win32":
        flags += ["-target", "x86_64-windows-gnu"]
    else:
        flags += ["-D_DEFAULT_SOURCE"]
    log = ["仅CPU decoder/probe及保护页测试；不访问GPU，不安装、不重启，不修改integration/primitive/docs，不commit。"]
    log += [f"SHA256 {digest} {p.relative_to(ROOT).as_posix()}" for p, digest in hashes.items()]

    def run(command):
        log.append("COMMAND " + subprocess.list2cmdline([str(x) for x in command]))
        result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                encoding="utf-8", errors="replace")
        log.extend([result.stdout, result.stderr, f"EXIT {result.returncode}"])
        print(result.stdout, end="", flush=True)
        print(result.stderr, end="", flush=True)
        if result.returncode:
            raise RuntimeError(f"命令失败，退出码{result.returncode}")

    status = 0
    try:
        run([str(zig), "version"])
        for mode, extra in [("strict", []), ("ndebug", ["-DNDEBUG"]),
                            ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
            binary = out / (mode + (".exe" if sys.platform == "win32" else ""))
            run([str(zig), "cc", *flags, *extra, str(source), str(probe), "-o", str(binary)])
            log.append(f"BINARY_SHA256 {hashlib.sha256(binary.read_bytes()).hexdigest()} {binary.name}")
            run([str(binary)])
            log.append("MODE PASS " + mode)
        if args.macos_ssh:
            verify_macos(files, hashes, log, stamp)
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise RuntimeError(f"测试期间源文件变化：{path}")
    except Exception as exc:
        status = 1
        log.append(f"FAIL {type(exc).__name__}: {exc}")
    log.append("RESULT " + ("PASS" if status == 0 else "FAIL"))
    (out / "results.txt").write_text("\n".join(log) + "\n", encoding="utf-8")
    print(log[-1] + " " + str(out), flush=True)
    return status


if __name__ == "__main__":
    sys.exit(main())
