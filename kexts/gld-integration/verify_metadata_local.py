"""本地隔离集成验证：不连接 SSH、不安装，测试替身不进入正式构建。"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", required=True, help="本地 Zig 可执行文件")
    args = parser.parse_args()
    out = ROOT / "out" / "integration-metadata-local"
    if not out.parent.is_dir():
        raise SystemExit("输出父目录不存在")
    out.mkdir(exist_ok=True)
    cc = [args.zig, "cc"]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
             "-Wconversion", "-Wshadow", "-O1", "-g", "-D__APPLE__=1",
             "-DkIOReturnBusy=((int32_t)0xe00002d5)",
             "-I" + str(HERE / "cpu-fixture"),
             "-I" + str(ROOT / "kexts/gld-objects/cpu-fixture")]
    if sys.platform == "win32":
        flags += ["-target", "x86_64-windows-gnu"]
    sources = ["gld-integration/integration.c", "gld-integration/context_exports.c",
               "gld-integration/render_exports.c", "gld-integration/tcl_exports.c",
               "gld-integration/dispatch_exports.c", "gld-context/vgpu_context.c",
               "gld-objects/vgpu_objects.c", "gld-texture/vgpu_texture.c",
               "gld-texture/vgpu_texture_bgra.c", "gld-framebuffer/vgpu_framebuffer.c",
               "gld-state/vgpu_clear_state.c", "gld-state/vgpu_triangle_state.c",
               "gld-pixel/gld_pixel.c"]
    log = []

    def run(command):
        result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace")
        log.extend([subprocess.list2cmdline(command), result.stdout, result.stderr,
                    f"EXIT {result.returncode}"])
        print(result.stdout, end="", flush=True)
        print(result.stderr, end="", flush=True)
        if result.returncode:
            raise RuntimeError("本地验证失败")

    try:
        sources += ["gld-integration/integration_metadata_test.c", "gld-objects/cpu-fixture/surface_fixture.c"]
        for mode, extra in [("strict", []), ("ndebug", ["-DNDEBUG"]),
                            ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
            objects = []
            for source in sources:
                obj = out / (pathlib.Path(source).stem + "-" + mode + ".o")
                strict = ["-Wstrict-prototypes", "-Wmissing-prototypes"] if "fixture" not in source else []
                run(cc + flags + extra + strict + ["-c", str(ROOT / "kexts" / source), "-o", str(obj)])
                objects.append(str(obj))
            exe = out / ("metadata-" + mode + (".exe" if sys.platform == "win32" else ""))
            run(cc + flags + extra + objects + ["-lpthread", "-o", str(exe)])
            run([str(exe)])
        print("PASS 严格对象编译、CPU 集成、NDEBUG、UBSan；未验证 macOS SDK/系统 CGL/真实 GPU")
    finally:
        (out / "results.txt").write_text("\n".join(log), encoding="utf-8")


if __name__ == "__main__":
    main()
