"""隔离上传并运行 triangle CPU/fault probe；不安装驱动、不访问 GPU。"""
import contextlib
from datetime import datetime, timezone
import hashlib
import io
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import mssh


def main():
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    remote = f"/var/tmp/vgpu-triangle-cpu-{stamp}"
    output = ROOT / "out" / f"triangle-cpu-{stamp}"
    output.mkdir(parents=True)
    log = io.StringIO()
    mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
    client = mssh.connect()

    def run(command):
        print(f"COMMAND {command}", file=log)
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            status = mssh.run(client, command, 120)
        print(f"EXIT {status}", file=log)
        if status:
            raise RuntimeError(f"CPU 验证命令退出 {status}")

    try:
        run(f"test -d /var/tmp && mkdir -p {remote}/gld-surface {remote}/gld-context")
        files = ["gld-surface/vgpu_surface.c", "gld-surface/vgpu_surface.h",
                 "gld-surface/triangle_fault_probe.c", "gld-context/vgpu_context.c",
                 "gld-context/vgpu_context.h"]
        for name in files:
            source = ROOT / "kexts" / name
            print(f"SHA256 {hashlib.sha256(source.read_bytes()).hexdigest()} {name}", file=log)
            with contextlib.redirect_stdout(log):
                if mssh.push(client, str(source), f"{remote}/{name}"):
                    raise RuntimeError(f"上传失败 {name}")
        flags = "-std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -pedantic -g -O1"
        run(f"cd {remote} && xcrun clang {flags} -c gld-surface/vgpu_surface.c -o surface.o && "
            f"xcrun clang {flags} -c gld-context/vgpu_context.c -o context.o")
        run(f"cd {remote} && xcrun clang {flags} -fsanitize=address,undefined "
            "-fno-omit-frame-pointer gld-surface/triangle_fault_probe.c "
            "-framework IOKit -framework CoreFoundation -pthread -o triangle_fault_probe && "
            "ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./triangle_fault_probe")
        run(f"cd {remote} && nm -g surface.o context.o")
        print("CPU_FAULT_ONLY_PASS; 未提交真实 GPU opcode 4", file=log)
    finally:
        client.close()
        (output / "results.txt").write_text(log.getvalue(), encoding="utf-8")
        print(log.getvalue())
        print(f"结果文件 {output / 'results.txt'}")


if __name__ == "__main__":
    main()
