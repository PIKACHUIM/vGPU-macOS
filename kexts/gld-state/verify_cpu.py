"""本地编译真实解码器；仅使用既有 IOReturn 类型头，不链接任何后端。"""
import argparse
import hashlib
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", help="已有本地 zig 可执行文件，默认 clang")
    parser.add_argument("--mutations", action="store_true", help="额外验证关键反例能杀死变异")
    args = parser.parse_args()
    out = ROOT / "out" / "gld-state-cpu"
    out.mkdir(exist_ok=True)
    cc = [args.zig, "cc"] if args.zig else ["clang"]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion",
             "-Wshadow", "-O1", "-g", "-I" + str(ROOT / "kexts/gld-objects/cpu-fixture"),
             "-I" + str(HERE)]
    if args.zig and sys.platform == "win32":
        flags += ["-target", "x86_64-windows-gnu"]
    elif sys.platform != "win32":
        flags += ["-D_DEFAULT_SOURCE"]
    source = HERE / "vgpu_clear_state.c"
    test = HERE / "clear_state_cpu_test.c"
    paths = [source, HERE / "vgpu_clear_state.h", test, pathlib.Path(__file__).resolve(),
             ROOT / "kexts/gld-context/vgpu_context.h", ROOT / "kexts/gld-surface/vgpu_surface.h",
             ROOT / "kexts/gld-objects/cpu-fixture/IOKit/IOReturn.h"]
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    log = ["仅本地 CPU 验证；无后端、远端、GPU、安装或 GLD 错误映射证明。"]
    log += [f"SHA256 {digest} {p.relative_to(ROOT).as_posix()}" for p, digest in hashes.items()]

    def run(cmd):
        log.append("COMMAND " + subprocess.list2cmdline([str(x) for x in cmd]))
        result = subprocess.run(cmd, cwd=ROOT, capture_output=True, encoding="utf-8", errors="replace")
        log.extend([result.stdout, result.stderr, f"EXIT {result.returncode}"])
        print(result.stdout, end="", flush=True)
        print(result.stderr, end="", flush=True)
        return result.returncode

    failed = 0
    suffix = ".exe" if sys.platform == "win32" else ""
    try:
        if run(cc + ["--version"]):
            raise RuntimeError("编译器不可用")
        for mode, extra in [("strict", []), ("ndebug", ["-DNDEBUG"]),
                            ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
            exe = out / (mode + suffix)
            if run(cc + flags + extra + [str(source), str(test), "-o", str(exe)]):
                failed += 1
                continue
            log.append(f"BINARY_SHA256 {hashlib.sha256(exe.read_bytes()).hexdigest()} {exe.name}")
            failed += bool(run([str(exe)]))
        if args.mutations:
            original = source.read_text(encoding="utf-8")
            mutations = {
                "srgb-wide": ("hb[0x3c1c] != 0", "load_u32(hb + 0x3c1c) != 0"),
                "summary-wide": ("load_u16(hb + 0x3cae) != 1", "(load_u16(hb + 0x3cae) != 1 || load_u32(hb + 0x3cae) != 1)"),
                "bitmap-bit0-only": ("load_u32(hb + 0x3e88) != 0", "(load_u32(hb + 0x3e88) & 1) != 0"),
                "integer-byte-only": ("load_u32(vb + 0x18) != 0", "vb[0x18] != 0"),
                "channel-boolean": ("& 1) == 0", ") == 0"),
                "all-targets-required": ("& 1) == 0", ") != 255"),
                "nan-accepted": ("if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000))", "if (0)"),
                "last-channel-unchecked": ("i < 4;", "i < 3;"),
                "partial-output": ("memcpy(&decoded.rgba[i], &bits, sizeof(bits));", "memcpy(&decoded.rgba[i], &bits, sizeof(bits)); memcpy(out, &decoded, sizeof(decoded));"),
                "alias-last-byte-missed": ("y < x + an;", "y < x + an - 1;"),
                "input-write": ("const unsigned char *hb = h.data, *vb = v.data;", "const unsigned char *hb = h.data, *vb = v.data; ((unsigned char *)h.data)[0] = 0;"),
            }
            for name, (before, after) in mutations.items():
                if original.count(before) != 1:
                    raise RuntimeError(f"变异锚点不唯一：{name}")
                mutant = out / (name + ".c")
                mutant.write_text(original.replace(before, after), encoding="utf-8")
                exe = out / (name + suffix)
                if run(cc + flags + [str(mutant), str(test), "-o", str(exe)]):
                    log.append(f"MUTATION INVALID {name}")
                    failed += 1
                elif run([str(exe)]) == 0:
                    log.append(f"MUTATION SURVIVED {name}")
                    failed += 1
                else:
                    log.append(f"MUTATION KILLED {name}")
                    print(log[-1], flush=True)
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise RuntimeError(f"验证期间源码变化：{path}")
    except Exception as exc:
        log.append(f"FAIL {type(exc).__name__}: {exc}")
        failed += 1
    log.append(f"RESULT {'PASS' if failed == 0 else 'FAIL'} failed={failed}")
    with (out / "results.txt").open("a", encoding="utf-8") as output:
        output.write("\n===== 本地 CPU 验证 =====\n" + "\n".join(log) + "\n")
    print(log[-1], flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    sys.exit(main())
