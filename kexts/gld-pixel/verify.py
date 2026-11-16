"""使用已有客体 clang；只写独占 kexts/gld-pixel 镜像，不安装。"""
import base64
import importlib.util
import pathlib
import shlex
import sys

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(ROOT / ".env.local")
files = {name: base64.b64encode((HERE / name).read_bytes()).decode()
         for name in ["gld_pixel.h", "gld_pixel.c"]}
files["gld_pixel_probe.c"] = base64.b64encode(
    (ROOT / "kexts/probes/gld_pixel_probe.c").read_bytes()).decode()
remote = '''
import base64, pathlib, subprocess, os
root = pathlib.Path('/var/tmp/vgpu-fb')
assert root.is_dir()
# 专用镜像，所有显式源码、临时文件及产物均限定在其中。
build = root / 'kexts/gld-pixel'
build.mkdir(parents=True, exist_ok=True)
files = FILES
for name, content in files.items():
    (build / name).write_bytes(base64.b64decode(content))
env = dict(os.environ, TMPDIR=str(build), CLANG_MODULE_CACHE_PATH=str(build / 'module-cache'))
def run(args):
    print('COMMAND ' + ' '.join(args), flush=True)
    result = subprocess.run(args, cwd=build, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='', flush=True)
    print('EXIT ' + str(result.returncode), flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
run(['clang', '--version'])
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-Wconversion', '-Wsign-conversion', '-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes', '-fno-common', '-I.']
run(['clang', *flags, '-O2', 'gld_pixel.c', 'gld_pixel_probe.c', '-o', 'gld_pixel_probe'])
run(['./gld_pixel_probe'])
run(['clang', *flags, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', 'gld_pixel.c', 'gld_pixel_probe.c', '-o', 'gld_pixel_probe_sanitized'])
env['ASAN_OPTIONS'] = 'halt_on_error=1:abort_on_error=1'
env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
run(['./gld_pixel_probe_sanitized'])
run(['clang', *flags, '--analyze', '-Xanalyzer', '-analyzer-output=text', 'gld_pixel.c'])
run(['nm', '-gU', 'gld_pixel_probe'])
'''.replace('FILES', repr(files))
client = mssh.connect()
try:
    _, stdout, stderr = client.exec_command('python3 -c ' + shlex.quote(remote), timeout=120)
    out = stdout.read().decode()
    err = stderr.read().decode()
    status = stdout.channel.recv_exit_status()
    (HERE / 'test-results.txt').write_text(out + err, encoding='utf-8')
    print(out + err)
    print('REMOTE_EXIT=' + str(status))
    sys.exit(status)
finally:
    client.close()
