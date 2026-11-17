#!/usr/bin/env python3
"""在客体独立临时目录编译并运行 ABI 门控测试，不安装或调用 GPU。"""
import pathlib
import shlex
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "tools"))
import mssh


def main():
    mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
    client = mssh.connect(retries=1)
    try:
        _, stdout, stderr = client.exec_command(
            "test -d /var/tmp && mktemp -d /var/tmp/vgpu-abi-gate.XXXXXX"
        )
        remote = stdout.read().decode().strip()
        error = stderr.read().decode()
        if stdout.channel.recv_exit_status() != 0 or not remote.startswith("/var/tmp/vgpu-abi-gate."):
            raise RuntimeError(error or "隔离目录创建失败")
        print("隔离测试目录:", remote)
        for name in ("abi_gate.c", "abi_gate.h", "abi_gate_test.c"):
            if mssh.push(client, str(HERE / name), remote + "/" + name) != 0:
                return 1
        root = shlex.quote(remote)
        command = (
            f"cd {root} && "
            "xcrun clang -std=c11 -Wall -Wextra -Werror abi_gate.c abi_gate_test.c -o abi_gate_test && "
            "./abi_gate_test && "
            "xcrun clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined "
            "abi_gate.c abi_gate_test.c -o abi_gate_sanitized && ./abi_gate_sanitized && "
            "xcrun clang -std=c11 -Wall -Wextra -Werror abi_gate.c abi_gate_test.c "
            "-framework OpenGL -o abi_gate_opengl && ./abi_gate_opengl"
        )
        return mssh.run(client, command, timeout=120)
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
