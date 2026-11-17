"""typed dispatch 专属纯 CPU 验证：严格警告、NDEBUG、UBSan，不连接 GPU。"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="Clang/GCC/Zig 编译器路径；zig 自动附加 cc")
    args = parser.parse_args()
    zig = ROOT / "out/.ziglang/ziglang/zig.exe"
    compiler = args.cc or (str(zig) if zig.exists() else shutil.which("clang"))
    if not compiler:
        parser.error("未找到 Zig/Clang，请显式传 --cc")
    command = [compiler]
    if pathlib.Path(compiler).stem == "zig":
        command += ["cc"]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
             "-Wconversion", "-Wshadow", "-Wstrict-prototypes",
             "-Wmissing-prototypes", "-O2", "-g"]
    if sys.platform == "win32" and pathlib.Path(compiler).stem == "zig":
        flags += ["-target", "x86_64-windows-gnu"]
    variants = [("strict", []), ("ndebug", ["-DNDEBUG"]),
                ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]
    with tempfile.TemporaryDirectory(prefix="vgpu-dispatch-cpu-") as temp:
        for name, extra in variants:
            exe = pathlib.Path(temp) / (name + (".exe" if sys.platform == "win32" else ""))
            print("构建并运行 dispatch CPU " + name, flush=True)
            subprocess.run([*command, *flags, *extra, str(HERE / "dispatch_exports.c"),
                            str(HERE / "dispatch_exports_probe.c"), "-o", str(exe)],
                           cwd=ROOT, check=True, timeout=180)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=60)
    print("全部 CPU 测试通过；未执行 GPU、Apple 私有函数或系统 GL/JIT。", flush=True)


if __name__ == "__main__":
    main()
