"""独立 TCL staging 纯 CPU 验证：strict、NDEBUG、UBSan，不连接设备。"""
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
    parser.add_argument("--cc", help="Clang 或 Zig 编译器路径")
    args = parser.parse_args()
    zig = ROOT / "out/.ziglang/ziglang/zig.exe"
    compiler = args.cc or (str(zig) if zig.exists() else shutil.which("clang"))
    if not compiler:
        parser.error("未找到 Zig/Clang，请传入 --cc")
    command = [compiler]
    is_zig = pathlib.Path(compiler).stem == "zig"
    if is_zig:
        command.append("cc")
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion",
             "-Wshadow", "-Wstrict-prototypes", "-Wmissing-prototypes", "-O2", "-g"]
    if sys.platform == "win32" and is_zig:
        flags += ["-target", "x86_64-windows-gnu"]
    injection = ["-Dcalloc=gi_tcl_test_calloc", "-Dfree=gi_tcl_test_free"]
    with tempfile.TemporaryDirectory(prefix="vgpu-tcl-cpu-") as temp:
        build = pathlib.Path(temp)
        for name, extra in [("strict", []), ("ndebug", ["-DNDEBUG"]),
                            ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
            obj = build / (name + ".o")
            exe = build / (name + (".exe" if sys.platform == "win32" else ""))
            print("构建并运行 TCL CPU " + name, flush=True)
            subprocess.run([*command, *flags, *extra, *injection, "-c", str(HERE / "tcl_exports.c"),
                            "-o", str(obj)], cwd=ROOT, check=True, timeout=180)
            subprocess.run([*command, *flags, *extra, str(HERE / "tcl_exports_probe.c"), str(obj),
                            "-o", str(exe)], cwd=ROOT, check=True, timeout=180)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=60)
    print("TCL strict/NDEBUG/UBSan 全部通过；未运行 GPU、公开 GL 或 JIT。", flush=True)


if __name__ == "__main__":
    main()
