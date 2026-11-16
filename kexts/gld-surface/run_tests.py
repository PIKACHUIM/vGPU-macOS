"""通过既有 mssh 连接运行；不输出认证信息，不修改驱动。"""
import importlib.util
import io
import contextlib
import pathlib
import sys
import hashlib
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
client = mssh.connect()
remote = "/var/tmp/vgpu-gld-surface-20261116"
log = io.StringIO()
log.write(f"RUN UTC {datetime.now(timezone.utc).isoformat()}\n")
log.write(f"SHA256 {hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()} run_tests.py\n")


def run(command, sudo=False):
    log.write(f"COMMAND sudo={sudo} {command}\n")
    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
        status = mssh.run(client, command, 120, use_sudo=sudo,
                          password=mssh.os.environ.get("VG_PASS"))
    log.write(f"EXIT {status}\n")
    if status:
        raise RuntimeError(f"客体命令退出 {status}")


try:
    run(f"test -d /var/tmp && mkdir -p {remote}")
    sources = [HERE / "vgpu_surface.h", HERE / "vgpu_surface.c",
               ROOT / "kexts/probes/gld_surface_probe.c"]
    fault = HERE / "fault_probe.c"
    if fault.exists():
        sources.append(fault)
    for source in sources:
        log.write(f"SHA256 {hashlib.sha256(source.read_bytes()).hexdigest()} {source.name}\n")
        with contextlib.redirect_stdout(log):
            if mssh.push(client, str(source), f"{remote}/{source.name}"):
                raise RuntimeError("上传失败")
    run("sw_vers; sysctl kern.boottime; clang --version; ioreg -r -c vgpuAccelerator -l")
    run("kmutil showloaded --list-only | /usr/bin/grep -i vgpu; /sbin/dmesg | /usr/bin/grep -E 'vgpu-fb.*(build|clear-only)' | /usr/bin/tail -12", True)
    run("dwarfdump --uuid /Library/Extensions/VGPUFramebuffer.kext/Contents/MacOS/vgpuFramebuffer; shasum -a 256 /Library/Extensions/VGPUFramebuffer.kext/Contents/MacOS/vgpuFramebuffer; strings /Library/Extensions/VGPUFramebuffer.kext/Contents/MacOS/vgpuFramebuffer | /usr/bin/grep -E '(78-|clear-only|gld-discovery)'")
    flags = "-std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -g -O1"
    framework = "-framework IOKit -framework CoreFoundation -pthread"
    build = f"cd {remote} && clang {flags} vgpu_surface.c gld_surface_probe.c {framework} -o surface-probe"
    log.write(build + "\n")
    run(build)
    run(f"{remote}/surface-probe", True)
    run(f"{remote}/surface-probe --expect-denied")
    sanitize = f"cd {remote} && clang {flags} -fsanitize=address,undefined -fno-omit-frame-pointer vgpu_surface.c gld_surface_probe.c {framework} -o surface-probe-sanitized"
    log.write(sanitize + "\n")
    run(sanitize)
    run(f"ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 {remote}/surface-probe-sanitized", True)
    if fault.exists():
        build_fault = f"cd {remote} && clang {flags} -fsanitize=address,undefined -fno-omit-frame-pointer -DIOConnectCallMethod=surface_test_call vgpu_surface.c fault_probe.c {framework} -o fault-probe"
        log.write(build_fault + "\n")
        run(build_fault)
        run(f"ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 {remote}/fault-probe", True)
    run("sysctl kern.boottime; /sbin/dmesg | /usr/bin/grep -E 'vgpu-fb.*(build|clear-only)' | /usr/bin/tail -18", True)
    log.write("ALL REQUESTED RUNS COMPLETED\n")
except Exception as exc:
    log.write(f"RUN FAILED {type(exc).__name__}: {exc}\n")
    raise
finally:
    client.close()
    with (HERE / "test-output.txt").open("a", encoding="utf-8") as output:
        output.write("\n" + log.getvalue())
    print(log.getvalue())
