"""仅本地平台 shim 验收：不是完整 macOS SDK 编译，不访问 GPU/远端。"""
from pathlib import Path
import argparse
import ast
import ctypes
import os
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--zig', help='已有本地 Zig 可执行文件，不下载工具')
parser.add_argument('--static-only', action='store_true', help='仅 Python 检查源码声明布局及独立 oracle，不调用编译器')
args = parser.parse_args()
source = Path(__file__).with_name('gld_resource_probe.c')
assert source.is_file()
assert Path(tempfile.gettempdir()).is_dir()
if args.static_only:
    text = source.read_text(encoding='utf-8')
    types = {'uint32_t': ctypes.c_uint32, 'uint64_t': ctypes.c_uint64,
             'unsigned char': ctypes.c_ubyte, 'float': ctypes.c_float}
    structures = {}
    for name, body in re.findall(r'struct (\w+)\s*\{([^{}]+)\};', text):
        if name not in ('create_request', 'resource_info', 'handle_request', 'release_response',
                        'submit_request', 'submit_response', 'read_request', 'read_response',
                        'vertex_input', 'clear_input'):
            continue
        fields = []
        for declaration in body.split(';'):
            declaration = declaration.strip()
            if not declaration:
                continue
            match = re.fullmatch(r'(uint32_t|uint64_t|unsigned char|float)\s+(.+)', declaration)
            assert match, declaration
            for field in match[2].split(','):
                field = field.strip().replace('ROW_BYTES', '1024')
                fname = re.match(r'\w+', field)[0]
                ftype = types[match[1]]
                for dimension in reversed(re.findall(r'\[(\d+)\]', field)):
                    ftype = ftype * int(dimension)
                fields.append((fname, ftype))
        structures[name] = type(name, (ctypes.Structure,), {'_fields_': fields})
    for name, size, alignment in re.findall(r'SIZE\((\w+),\s*(\d+),\s*(\d+)\)', text):
        assert ctypes.sizeof(structures[name]) == int(size), name
        assert ctypes.alignment(structures[name]) == int(alignment), name
    for name, field, offset in re.findall(r'OFFSET\((\w+),\s*(\w+),\s*(\d+)\)', text):
        if name in structures:
            assert getattr(structures[name], field).offset == int(offset), (name, field)
    for name in re.findall(r'HEADER\((\w+)\)', text):
        if name in structures:
            assert structures[name].version.offset == 0 and structures[name].byteSize.offset == 4
    screen_cases = re.findall(r'(\{\{\d+,\d+\},\{\d+,\d+\},\{\d+,\d+\}\}),\s*(\d+)\}', text)
    assert len(screen_cases) == 3
    for screen, expected in screen_cases:
        vertices = ast.literal_eval(screen.replace('{', '[').replace('}', ']'))
        count = 0
        for y in range(256):
            for x in range(256):
                edges = []
                for i in range(3):
                    a, b = vertices[i], vertices[(i+1) % 3]
                    edges.append((b[0]-a[0])*(2*y+1-a[1]) - (b[1]-a[1])*(2*x+1-a[0]))
                assert all(edges), (x, y)
                count += all(e < 0 for e in edges) or all(e > 0 for e in edges)
        assert count == int(expected), (count, expected)
        print('PASS Python 独立 oracle：', count, '背景：', 65536-count)
    print('PASS 源码提取的十个结构布局和三组 oracle；不是 C 编译或 C 执行，不验证 SDK/GPU。')
    raise SystemExit(0)
if not args.zig:
    parser.error('非 --static-only 模式需要 --zig')

# 符号值仅供本地替身链接；不模拟 macOS IOKit 错误码、驱动或 GPU。
header = r'''
#ifndef PROBE_LOCAL_IOKIT_SHIM
#define PROBE_LOCAL_IOKIT_SHIM
#include <stddef.h>
#include <stdint.h>
typedef int32_t IOReturn;
typedef uint32_t io_connect_t;
typedef uint32_t io_service_t;
typedef void *CFMutableDictionaryRef;
enum { kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnUnsupported=2,
       kIOReturnNotReady=3, kIOReturnNoResources=4, kIOReturnNotPermitted=5,
       kIOReturnNotFound=6, IO_OBJECT_NULL=0, MACH_PORT_NULL=0 };
CFMutableDictionaryRef IOServiceMatching(const char *);
io_service_t IOServiceGetMatchingService(uint32_t, CFMutableDictionaryRef);
IOReturn IOServiceOpen(io_service_t, uint32_t, uint32_t, io_connect_t *);
IOReturn IOObjectRelease(io_service_t);
IOReturn IOServiceClose(io_connect_t);
uint32_t mach_task_self(void);
IOReturn IOConnectCallMethod(io_connect_t, uint32_t, const uint64_t *, uint32_t,
    const void *, size_t, uint64_t *, uint32_t *, void *, size_t *);
#endif
'''
stub = r'''
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
static void forbidden(void) { fputs("禁止的替身调用\n", stderr); exit(97); }
CFMutableDictionaryRef IOServiceMatching(const char *s) { (void)s; return (void *)1; }
io_service_t IOServiceGetMatchingService(uint32_t p, CFMutableDictionaryRef d)
{ (void)p; (void)d; return IO_OBJECT_NULL; }
IOReturn IOServiceOpen(io_service_t s, uint32_t t, uint32_t y, io_connect_t *c)
{ (void)s; (void)t; (void)y; (void)c; forbidden(); return 99; }
IOReturn IOObjectRelease(io_service_t s) { (void)s; forbidden(); return 99; }
IOReturn IOServiceClose(io_connect_t c) { (void)c; forbidden(); return 99; }
uint32_t mach_task_self(void) { forbidden(); return 0; }
IOReturn IOConnectCallMethod(io_connect_t c, uint32_t s, const uint64_t *i, uint32_t n,
    const void *p, size_t z, uint64_t *o, uint32_t *m, void *b, size_t *l)
{ (void)c; (void)s; (void)i; (void)n; (void)p; (void)z; (void)o; (void)m;
  (void)b; (void)l; forbidden(); return 99; }
'''
with tempfile.TemporaryDirectory(prefix='gld-resource-probe-') as directory:
    work = Path(directory)
    (work / 'IOKit').mkdir()
    (work / 'IOKit/IOKitLib.h').write_text(header, encoding='utf-8')
    (work / 'stub.c').write_text(stub, encoding='utf-8')
    env = os.environ.copy()
    env['ZIG_GLOBAL_CACHE_DIR'] = str(work / 'global-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = str(work / 'local-cache')
    executable = work / 'probe.exe'
    command = [args.zig, 'cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
               '-I', str(work), str(source), str(work / 'stub.c'), '-o', str(executable)]
    print('本地平台 shim 构建命令：', command, flush=True)
    subprocess.run(command, env=env, check=True)
    subprocess.run([str(executable), '--oracle-only'], check=True)
    stopped = subprocess.run([str(executable)], capture_output=True)
    assert stopped.returncode == 1, stopped
    assert b'FAIL resource probe' in stopped.stdout, stopped.stdout
    assert b'IOServiceOpen' not in stopped.stderr, stopped.stderr
    usage = subprocess.run([str(executable), '--invalid'], capture_output=True)
    assert usage.returncode == 2
    print('PASS C11/所有 sizeof+offsetof 断言、CPU oracle、缺服务立即停止、非法参数；无 GPU 调用。')
    print('未完成真实 macOS SDK 编译或实机 GPU 验收。')
