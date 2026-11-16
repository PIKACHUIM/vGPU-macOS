"""在独占客体目录严格构建、动态加载自研导出；不安装、不调用系统GLD。"""
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
names = ["gld_pixel.c", "gld_pixel.h", "gld_pixel_adapter.c", "gld_pixel_adapter.h",
         "gld_pixel_exports.c", "gld_pixel_exports.h"]
files = {name: base64.b64encode((HERE / name).read_bytes()).decode() for name in names}
for name in ["gld_pixel_probe.c", "gld_pixel_adapter_probe.c", "gld_pixel_exports_probe.c"]:
    files[name] = base64.b64encode((ROOT / "kexts/probes" / name).read_bytes()).decode()
remote = r'''
import base64, hashlib, os, pathlib, subprocess
root = pathlib.Path('/var/tmp/vgpu-fb')
assert root.is_dir()
build = root / 'kexts/gld-pixel-exports'
build.mkdir(parents=True, exist_ok=True)
files = FILES
for name, content in files.items():
    data = base64.b64decode(content)
    (build / name).write_bytes(data)
    print('SHA256 ' + name + ' ' + hashlib.sha256(data).hexdigest(), flush=True)
env = dict(os.environ, TMPDIR=str(build), CLANG_MODULE_CACHE_PATH=str(build / 'module-cache'))
env['ASAN_OPTIONS'] = 'halt_on_error=1:abort_on_error=1'
env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
def run(args):
    print('COMMAND ' + ' '.join(args), flush=True)
    result = subprocess.run(args, cwd=build, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='', flush=True)
    print('EXIT ' + str(result.returncode), flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
run(['sw_vers'])
run(['clang', '--version'])
sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
header = (pathlib.Path(sdk) / 'System/Library/Frameworks/OpenGL.framework/Headers/CGLTypes.h').resolve()
print('SDK ' + str(header))
for index, line in enumerate(header.read_text().splitlines(), 1):
    if any(name in line for name in ['kCGLNoError ', 'kCGLBadAttribute ', 'kCGLBadAlloc ', 'kCGLBadMatch ', 'kCGLBadPixelFormat ', 'kCGLBadValue ']):
        print(str(index) + ': ' + line)
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-Wconversion', '-Wsign-conversion', '-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes', '-fno-common', '-pthread', '-I.']
sources = ['gld_pixel.c', 'gld_pixel_adapter.c', 'gld_pixel_exports.c']
for suffix, optimize in [('', ['-O2']), ('_sanitized', ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
    common = ['clang', *flags, *optimize]
    library = 'libgld_pixel_exports' + suffix + '.dylib'
    run([*common, '-fvisibility=hidden', '-dynamiclib', *sources, '-o', library])
    run(['nm', '-gU', library])
    probe = 'exports_probe' + suffix
    run([*common, 'gld_pixel_exports_probe.c', '-o', probe])
    run(['./' + probe, './' + library])
    # 失败注入只编译进另一份测试dylib，正式dylib不含该控制入口。
    objects = []
    for source in sources:
        obj = pathlib.Path(source).stem + '_fault' + suffix + '.o'
        run([*common, '-fvisibility=hidden', '-Dmalloc=gpx_test_malloc', '-Dcalloc=gpx_test_calloc', '-c', source, '-o', obj])
        objects.append(obj)
    injection = 'injection' + suffix + '.o'
    run([*common, '-fvisibility=hidden', '-DGPX_FAULT_ALLOCATOR', '-c', 'gld_pixel_exports_probe.c', '-o', injection])
    fault_library = 'libgld_pixel_fault' + suffix + '.dylib'
    run([*common, '-dynamiclib', *objects, injection, '-o', fault_library])
    run(['./' + probe, './' + fault_library, 'fault'])
    for regression, regression_sources in [
        ('adapter_probe', ['gld_pixel.c', 'gld_pixel_adapter.c', 'gld_pixel_adapter_probe.c']),
        ('pixel_probe', ['gld_pixel.c', 'gld_pixel_probe.c'])]:
        run([*common, *regression_sources, '-o', regression + suffix])
        run(['./' + regression + suffix])
for source in sources:
    run(['clang', *flags, '--analyze', '-Xanalyzer', '-analyzer-output=text', source])
'''.replace('FILES', repr(files))
client = mssh.connect()
try:
    _, stdout, stderr = client.exec_command('python3 -c ' + shlex.quote(remote), timeout=240)
    out = stdout.read().decode()
    err = stderr.read().decode()
    status = stdout.channel.recv_exit_status()
    (HERE / 'exports-test-results.txt').write_text(out + err, encoding='utf-8')
    print(out + err)
    print('REMOTE_EXIT=' + str(status))
    sys.exit(status)
finally:
    client.close()
