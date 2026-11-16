"""独立 position-only / position+color CPU runner：严格警告、UBSan、NDEBUG；不连接设备。"""
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
    parser.add_argument("--cc", help="编译器路径；zig 自动附加 cc")
    args = parser.parse_args()
    zig = ROOT / "out/.ziglang/ziglang/zig.exe"
    compiler = args.cc or (str(zig) if zig.exists() else shutil.which("clang"))
    if not compiler:
        parser.error("未找到 Zig/Clang，请显式传 --cc")
    command = [compiler]
    if pathlib.Path(compiler).stem == "zig":
        command += ["cc"]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion",
             "-Wshadow", "-Wstrict-prototypes", "-Wmissing-prototypes", "-O2", "-g"]
    # macOS 使用真实 SDK IOReturn；其他宿主仅用声明 fixture，不冒充 SDK 验收。
    if sys.platform != "darwin":
        flags += ["-I" + str(ROOT / "kexts/gld-objects/cpu-fixture")]
    if sys.platform == "win32" and pathlib.Path(compiler).stem == "zig":
        flags += ["-target", "x86_64-windows-gnu"]
    injection = ["-Dmalloc=gi_primitive_test_malloc", "-Dcalloc=gi_primitive_test_calloc",
                 "-Dfree=gi_primitive_test_free"]
    with tempfile.TemporaryDirectory(prefix="vgpu-primitive-cpu-") as temp:
        build = pathlib.Path(temp)
        for name, extra in [("strict", []), ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"]),
                            ("ndebug", ["-DNDEBUG"])]:
            obj = build / (name + ".o")
            exe = build / (name + (".exe" if sys.platform == "win32" else ""))
            print("构建并运行 primitive CPU " + name, flush=True)
            subprocess.run([*command, *flags, *extra, *injection, "-c", str(HERE / "primitive_exports.c"),
                            "-o", str(obj)], cwd=ROOT, check=True, timeout=180)
            subprocess.run([*command, *flags, *extra, str(HERE / "primitive_exports_probe.c"), str(obj),
                            "-o", str(exe)], cwd=ROOT, check=True, timeout=180)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=60)
    print("全部 CPU 测试通过；未验证 GPU、系统 GLD 返回协议或 JIT。", flush=True)


if __name__ == "__main__":
    main()
