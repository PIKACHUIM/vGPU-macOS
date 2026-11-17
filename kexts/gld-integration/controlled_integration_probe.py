"""真实 SDK 的隔离 CPU 验证；不安装、不执行 GPU 或公开 CGL。"""
import contextlib
from datetime import datetime, timezone
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
K = ROOT / 'kexts'
spec = importlib.util.spec_from_file_location('mssh', ROOT / 'tools/mssh.py')
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
local = ROOT / 'out' / ('controlled-integration-' + stamp)
assert local.parent.is_dir()
local.mkdir()
remote = '/var/tmp/controlled-integration-' + stamp
log = io.StringIO()
client = mssh.connect(retries=1)


def run(command):
    print(command, flush=True)
    log.write('COMMAND ' + command + '\n')
    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
        rc = mssh.run(client, command, 120)
    log.write('EXIT ' + str(rc) + '\n')
    (local / 'validation.log').write_text(log.getvalue(), encoding='utf-8')
    if rc:
        raise RuntimeError('远端失败，详见 ' + str(local / 'validation.log'))


try:
    run('uname -a; sw_vers; sysctl -n kern.boottime; xcrun clang --version; xcrun --show-sdk-path')
    run('test -d /var/tmp && mkdir ' + shlex.quote(remote))
    files = []
    for directory in ['gld-integration', 'gld-device', 'gld-pixel', 'gld-context',
                      'gld-objects', 'gld-texture', 'gld-surface', 'gld-framebuffer', 'gld-state']:
        files += [p for p in (K / directory).iterdir() if p.is_file() and
                  (p.suffix == '.h' or (p.suffix == '.c' and
                   ('probe' not in p.stem and 'test' not in p.stem or p.name in
                    ('probe.c', 'controlled_integration_probe.c', 'render_exports_probe.c',
                     'integration_metadata_test.c', 'integration_read_test.c'))))]
    files += [K / 'gld-integration' / n for n in ('build.sh', 'Info.plist', 'traps.S', 'controlled_integration_probe.py')]
    files += [K / 'gld-diagnostic/required_entries.inc', K / 'gld-objects/cpu-fixture/surface_fixture.c']
    manifest = {}
    with client.open_sftp() as sftp:
        dirs = {remote}
        for p in files:
            rel = p.relative_to(K).as_posix()
            parent = remote
            for part in Path(rel).parts[:-1]:
                parent += '/' + part
                if parent not in dirs:
                    sftp.mkdir(parent)
                    dirs.add(parent)
            data = p.read_bytes()
            manifest[rel] = hashlib.sha256(data).hexdigest()
            with sftp.file(remote + '/' + rel, 'wb') as f:
                f.write(data)
    (local / 'source-sha256.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    run(f'bash {remote}/gld-integration/build.sh {remote}/default')
    run(f'bash {remote}/gld-integration/build.sh {remote}/candidate --controlled-public-experiment')
    run(f'{remote}/default/integration_probe {remote}/default/VGPUDriverIntegration.bundle/Contents/MacOS/VGPUDriverIntegration --empty')
    sources = ['gld-integration/integration.c', 'gld-integration/context_exports.c',
               'gld-integration/render_exports.c', 'gld-integration/tcl_exports.c',
               'gld-integration/dispatch_exports.c', 'gld-state/vgpu_triangle_state.c',
               'gld-context/vgpu_context.c', 'gld-objects/vgpu_objects.c',
               'gld-texture/vgpu_texture.c', 'gld-texture/vgpu_texture_bgra.c',
               'gld-pixel/gld_pixel.c', 'gld-framebuffer/vgpu_framebuffer.c', 'gld-state/vgpu_clear_state.c']
    flags = '-arch x86_64 -mmacosx-version-min=15.0 -std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow -O1 -g'
    for mode, extra in [('strict', ''), ('asan', '-fsanitize=address -fno-omit-frame-pointer'),
                        ('ubsan', '-fsanitize=undefined -fno-sanitize-recover=all')]:
        objects = []
        commands = []
        for i, source in enumerate(sources):
            obj = f'{remote}/default/cpu-{mode}-{i}.o'
            commands.append(f'xcrun clang {flags} -Wstrict-prototypes -Wmissing-prototypes {extra} -c {remote}/{source} -o {obj}')
            objects.append(obj)
        run(' && '.join(commands))
        for name, probes in [('controlled', ['gld-integration/controlled_integration_probe.c']),
                             ('metadata', ['gld-integration/integration_metadata_test.c', 'gld-objects/cpu-fixture/surface_fixture.c']),
                             ('read', ['gld-integration/integration_read_test.c'])]:
            target = f'{remote}/default/{name}-{mode}'
            # 旧 fixture 保留其已有告警边界；生产单元已严格独立编译。
            strict = '' if name == 'metadata' else '-Wstrict-prototypes -Wmissing-prototypes'
            run(f'xcrun clang {flags} {strict} {extra} ' + ' '.join(objects) + ' ' +
                ' '.join(remote + '/' + p for p in probes) + f' -o {target} && {target}')
        candidate_objects = []
        commands = []
        for i, source in enumerate(sources):
            obj = f'{remote}/default/candidate-{mode}-{i}.o'
            commands.append(f'xcrun clang {flags} -Wstrict-prototypes -Wmissing-prototypes {extra} -DGI_CONTROLLED_PUBLIC_EXPERIMENT=1 -c {remote}/{source} -o {obj}')
            candidate_objects.append(obj)
        run(' && '.join(commands))
        target = f'{remote}/default/controlled-public-{mode}'
        run(f'xcrun clang {flags} -Wstrict-prototypes -Wmissing-prototypes {extra} -DGI_CONTROLLED_PUBLIC_EXPERIMENT=1 ' +
            ' '.join(candidate_objects) +
            f' {remote}/gld-integration/controlled_integration_probe.c -o {target} && {target}')
    run(f'shasum -a 256 {remote}/default/*-strict {remote}/default/*-asan {remote}/default/*-ubsan {remote}/default/VGPUDriverIntegration.bundle/Contents/MacOS/VGPUDriverIntegration {remote}/candidate/VGPUDriverControlledCandidate.bundle/Contents/MacOS/VGPUDriverIntegration')
    run(f'otool -L {remote}/default/controlled-strict; sysctl -n kern.boottime')
    for rel, digest in manifest.items():
        if hashlib.sha256((K / rel).read_bytes()).hexdigest() != digest:
            raise RuntimeError('回归期间源码发生变化，暂停交付：' + rel)
    log.write('PASS 源码哈希与回归快照一致\n')
    # 完整产物留在唯一远端隔离目录；日志已包含全部命令、退出码与哈希。
    # 不回收数十个 sanitizer 对象，避免 SFTP 下载掩盖实际测试完成状态。
    print('PASS ' + str(local), flush=True)
finally:
    (local / 'validation.log').write_text(log.getvalue(), encoding='utf-8')
    (local / 'remote.txt').write_text(remote + '\n', encoding='utf-8')
    client.close()
