"""编译真实 FBO/context 与隔离 CPU surface；不上传、不运行 GPU。"""
import argparse
import hashlib
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", help="Zig 编译器路径；省略时使用 clang")
    args = parser.parse_args()
    cc = [args.zig, "cc"] if args.zig else ["clang"]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion", "-Wshadow", "-O1", "-g"]
    if args.zig and sys.platform == "win32":
        flags += ["-target", "x86_64-windows-gnu"]
    if sys.platform != "darwin":
        flags += ["-I" + str(ROOT / "kexts/gld-objects/cpu-fixture")]
    sources = [HERE / "vgpu_framebuffer.c", ROOT / "kexts/gld-context/vgpu_context.c",
               HERE / "test_surface.c", HERE / "test_framebuffer.c"]
    inputs = sources + [HERE / "vgpu_framebuffer.h", HERE / "verify_cpu.py",
                       ROOT / "kexts/gld-context/vgpu_context.h"]
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    log = ["真实 FBO/context 源码；双槽 CPU surface 替身；不是系统 GLD 或 GPU 验证。"]
    failed = 0

    def run(command):
        log.append("COMMAND " + subprocess.list2cmdline([str(x) for x in command]))
        r = subprocess.run(command, capture_output=True, text=True, errors="replace")
        log.extend([r.stdout, r.stderr, f"EXIT {r.returncode}"])
        print(r.stdout + r.stderr, end="", flush=True)
        return r.returncode == 0

    for name, extra in [("strict", []), ("ndebug", ["-DNDEBUG"]),
                        ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
        binary = ROOT / "out" / ("gld-framebuffer-" + name + (".exe" if sys.platform == "win32" else ""))
        if not run(cc + flags + extra + [str(p) for p in sources] + ["-lpthread", "-o", str(binary)]):
            failed += 1
        elif not run([str(binary)]):
            failed += 1
    for path, digest in hashes.items():
        log.append(f"SHA256 {digest} {path.relative_to(ROOT).as_posix()}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            log.append(f"FAIL 验证期间源码改变 {path}")
            failed += 1
    log.append(f"RESULT {'PASS' if not failed else 'FAIL'} failed={failed}")
    (HERE / "cpu-results.txt").write_text("\n".join(log) + "\n", encoding="utf-8")
    print(log[-1])
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
