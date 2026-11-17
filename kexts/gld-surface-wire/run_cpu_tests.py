"""复用现有客体 clang，仅执行 CPU 测试；不安装、不访问 GPU。"""
import contextlib
import hashlib
import importlib.util
import io
import pathlib
import shlex
import sys
import uuid
from datetime import datetime, timezone

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEPS = ROOT / "kexts/gld-integration/.test-deps"
if DEPS.is_dir():
    sys.path.insert(0, str(DEPS))
log = io.StringIO()
client = None
success = False


def run(command):
    log.write("COMMAND " + command + "\n")
    _, stdout, stderr = client.exec_command(command, timeout=120)
    output = stdout.read().decode("utf-8", "replace")
    errors = stderr.read().decode("utf-8", "replace")
    status = stdout.channel.recv_exit_status()
    log.write(output + errors + f"EXIT {status}\n")
    if status:
        raise RuntimeError("CPU 编译或测试失败")
    return output


try:
    log.write(f"UTC {datetime.now(timezone.utc).isoformat()}\n")
    spec = importlib.util.spec_from_file_location("mssh", ROOT / "tools/mssh.py")
    mssh = importlib.util.module_from_spec(spec)
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        spec.loader.exec_module(mssh)
        mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
        client = mssh.connect()
    remote = "/var/tmp/vgpu-surface-wire-cpu-" + uuid.uuid4().hex
    run("test -d /var/tmp && mkdir " + shlex.quote(remote))
    sources = ["vgpu_surface_wire.h", "vgpu_surface_wire.c", "wire_test.c"]
    with client.open_sftp() as sftp:
        for name in sources:
            path = HERE / name
            log.write(f"SHA256 {hashlib.sha256(path.read_bytes()).hexdigest()} {name}\n")
            sftp.put(str(path), remote + "/" + name)
    log.write("SHA256 " + hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
              + " run_cpu_tests.py\n")
    run("clang --version && xcrun --show-sdk-path")
    prefix = "cd " + shlex.quote(remote) + " && "
    run(prefix + "shasum -a 256 " + " ".join(sources))
    strict = "-Wall -Wextra -Werror -Wconversion -Wshadow -Wstrict-prototypes -pedantic -g -O1"
    cflags = "-std=c11 " + strict
    core = "vgpu_surface_wire.c"
    test = "wire_test.c"
    run(prefix + f"clang {cflags} {core} {test} -o wire-test")
    run(prefix + "./wire-test")
    run(prefix + f"clang {cflags} -fsanitize=address,undefined -fno-omit-frame-pointer "
        + f"{core} {test} -o wire-sanitized")
    run(prefix + "ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./wire-sanitized")
    run(prefix + "clang++ -x c++ -std=c++11 -Wall -Wextra -Werror -Wconversion -Wshadow "
        + f"-pedantic -O1 -c {core} -o wire-cxx.o")
    run(prefix + f"clang {cflags} -c {test} -o test-c.o")
    run(prefix + "clang++ wire-cxx.o test-c.o -o wire-cxx-test && ./wire-cxx-test")
    run(prefix + f"clang {cflags} -ffreestanding -fno-builtin -fno-stack-protector "
        + f"-c {core} -o wire-freestanding.o")
    undefined = run(prefix + "nm -u wire-freestanding.o")
    if undefined.strip():
        raise RuntimeError("freestanding 生产对象存在外部依赖")
    log.write("ALL CPU RUNS PASSED; no undefined symbols in freestanding core\n")
    success = True
except BaseException as exc:
    # 不输出连接异常正文，以免泄漏连接凭据。
    log.write("FAILED " + type(exc).__name__ + "\n")
finally:
    if client is not None:
        client.close()
    (HERE / "cpu-test-results.txt").write_text(log.getvalue(), encoding="utf-8")
    print(log.getvalue(), end="")
sys.exit(0 if success else 1)
