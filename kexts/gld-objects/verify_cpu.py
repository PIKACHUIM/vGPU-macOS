"""直接编译真实源码；仅 surface/IOReturn 平台边界用显式 CPU fixture。无上传。"""
import argparse
import hashlib
import pathlib
import platform
import subprocess
import sys
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", help="便携 zig 可执行文件；省略时使用 clang")
    parser.add_argument("--regressions-only", action="store_true", help="独立运行P2回归，保留各项失败证据")
    parser.add_argument("--label", default="未标注", help="追加日志的阶段标签")
    args = parser.parse_args()
    out = ROOT / "out" / "gld-objects-cpu"
    out.mkdir(exist_ok=True)
    cc = [args.zig, "cc"] if args.zig else ["clang"]
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Wconversion", "-Wshadow", "-O1", "-g"]
    if args.zig and sys.platform == "win32":
        flags += ["-target", "x86_64-windows-gnu"]
    groups = {
        "texture": ["gld-texture/vgpu_texture.c", "probes/gld_texture_probe.c"],
        "objects": ["gld-context/vgpu_context.c", "gld-objects/vgpu_objects.c",
                    "gld-objects/cpu-fixture/surface_fixture.c", "probes/gld_objects_probe.c"],
        "objects-import": ["gld-objects/cpu-fixture/context_import_fixture.c",
                           "gld-objects/vgpu_objects.c", "gld-objects/cpu-fixture/surface_fixture.c",
                           "probes/gld_objects_import_probe.c"],
    }
    paths = {ROOT / "kexts" / p for values in groups.values() for p in values}
    paths.update([HERE / "vgpu_objects.h", HERE / "verify_cpu.py",
                  HERE / "cpu-fixture/IOKit/IOReturn.h",
                  ROOT / "kexts/gld-context/vgpu_context.h",
                  ROOT / "kexts/gld-surface/vgpu_surface.h",
                  ROOT / "kexts/gld-texture/vgpu_texture.h"])
    before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    log = [f"UTC {datetime.now(timezone.utc).isoformat()}", f"HOST {platform.platform()}",
           f"LABEL {args.label}", f"REGRESSIONS_ONLY {args.regressions_only}",
           "实际 objects/context/texture 源模块；仅 surface 是 CPU fixture；非系统 GLD/GPU 验证。",
           "历史780记录未在仓库找到；本日志只证明当前源文件的本地运行。"]
    for p, digest in sorted(before.items()):
        log.append(f"SHA256 {digest} {p.relative_to(ROOT).as_posix()}")
    failed = 0

    def run(cmd):
        log.append("COMMAND " + subprocess.list2cmdline([str(x) for x in cmd]))
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, encoding="utf-8", errors="replace")
        log.extend([r.stdout, r.stderr, f"EXIT {r.returncode}"])
        print(r.stdout, end="", flush=True)
        print(r.stderr, end="", flush=True)
        return r.returncode == 0

    try:
        if not run(cc + ["--version"]):
            failed += 1
        for mode, extra in [("strict", []), ("ndebug", ["-DNDEBUG"]),
                            ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
            for group, sources in groups.items():
                exe = out / (group + "-" + mode + (".exe" if sys.platform == "win32" else ""))
                include = ["-I" + str(HERE / "cpu-fixture")] if group.startswith("objects") else []
                cmd = cc + flags + extra + include + [str(ROOT / "kexts" / p) for p in sources]
                if not run(cmd + ["-lpthread", "-o", str(exe)]):
                    failed += 1
                    continue
                log.append(f"BINARY_SHA256 {hashlib.sha256(exe.read_bytes()).hexdigest()} {exe.name}")
                cases = {"texture": ["create-output", "share-output", "cpu-dirty-overlap"],
                         "objects": ["sampler-targets", "create-output", "context-output"],
                         "objects-import": [None]}
                for case in cases[group] if args.regressions_only else [None]:
                    if not run([str(exe)] + ([case] if case else [])):
                        failed += 1
        for p, digest in before.items():
            if hashlib.sha256(p.read_bytes()).hexdigest() != digest:
                log.append(f"FAIL 源文件在验证期间改变 {p}")
                failed += 1
    except Exception as exc:
        log.append(f"FAIL {type(exc).__name__}: {exc}")
        failed += 1
    log.append(f"RESULT {'PASS' if not failed else 'FAIL'} failed={failed}")
    log.append("未验证：macOS ABI、系统 GLD 初始化、shader 编译、Metal、真实 GPU 工作、ASan/LSan。")
    with (HERE / "cpu-results.txt").open("a", encoding="utf-8") as f:
        f.write("\n===== 本地 CPU 验证 =====\n" + "\n".join(log) + "\n")
    print(log[-2], flush=True)
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
