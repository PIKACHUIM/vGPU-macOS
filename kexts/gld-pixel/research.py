"""只读提取实际驱动证据；结果仅写入本目录。"""
import importlib.util
import pathlib
import shlex
import sys

sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mssh", ROOT / "tools/mssh.py")
mssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mssh)
mssh.load_env_file(ROOT / ".env.local")
remote = r'''
import ctypes, struct, subprocess
paths = [
('/System/Library/Extensions/AppleIntelKBLGraphicsGLDriver.bundle/Contents/MacOS/AppleIntelKBLGraphicsGLDriver', ['_gldChoosePixelFormat:', '_glrSanitizePixelFormat:', '_gldDestroyPixelFormat:']),
('/System/Library/PrivateFrameworks/GPUSupport.framework/Versions/A/Libraries/libGPUSupportMercury.dylib', ['_gpumChoosePixelFormat:', '_gpumDestroyPixelFormat:'])]
for path, labels in paths:
    print(subprocess.check_output(['dyld_info', '-uuid', path], text=True))
    text = subprocess.check_output(['dyld_info', '-disassemble', path], text=True)
    for label in labels:
        lines = text.splitlines()
        start = lines.index(label)
        end = next((i for i in range(start + 1, len(lines)) if lines[i].endswith(':') and not lines[i].startswith('0x')), len(lines))
        if 'DestroyPixelFormat' in label:
            end = next(i + 1 for i in range(start + 1, end) if 'retq' in lines[i] or 'jmp      _gpumDestroyPixelFormat' in lines[i])
        print('\n'.join(lines[start:end]))
p = paths[1][0]
lib = ctypes.CDLL(p)
entry = ctypes.cast(lib.gpumChoosePixelFormat, ctypes.c_void_p).value
# 通过 dladdr 找映像基址并核验 UUID，再按已核验映像内偏移读取跳转表。
class Info(ctypes.Structure):
    _fields_ = [('fname', ctypes.c_char_p), ('base', ctypes.c_void_p), ('sname', ctypes.c_char_p), ('saddr', ctypes.c_void_p)]
system = ctypes.CDLL(None)
system.dladdr.argtypes = [ctypes.c_void_p, ctypes.POINTER(Info)]
system.dladdr.restype = ctypes.c_int
info = Info()
assert system.dladdr(entry, ctypes.byref(info))
header = ctypes.string_at(info.base, 32)
magic, cpu, subtype, kind, ncmds, size, flags, reserved = struct.unpack('<8I', header)
assert magic == 0xfeedfacf and cpu == 0x1000007 and size < 1048576
commands = ctypes.string_at(info.base + 32, size)
off = 0
uuids = []
for i in range(ncmds):
    cmd, length = struct.unpack_from('<II', commands, off)
    assert length >= 8 and off + length <= len(commands)
    if cmd == 0x1b:
        assert length == 24
        uuids.append(commands[off+8:off+24].hex())
    off += length
assert off == size and uuids == ['f206ce07bca73cc7a494112686cde945'], uuids
slide = entry - 0x7ff918355614
assert ctypes.string_at(entry, 4).hex() == '554889e5'
print('TABLE_GATE UUID=' + uuids[0] + ' symbol=gpumChoosePixelFormat arch=x86_64')
for table, first, count in [(0x7ff918355dec, 3, 12), (0x7ff918355e1c, 51, 40)]:
    raw = ctypes.string_at(table + slide, count * 4)
    for i in range(count):
        delta = struct.unpack_from('<i', raw, i * 4)[0]
        print('TOKEN %d cell=0x%x bytes=%s target=0x%x' % (first+i, table+4*i, raw[4*i:4*i+4].hex(), table+delta))
'''
client = mssh.connect()
try:
    _, stdout, stderr = client.exec_command("python3 -c " + shlex.quote(remote), timeout=90)
    out = stdout.read().decode()
    err = stderr.read().decode()
    status = stdout.channel.recv_exit_status()
    (pathlib.Path(__file__).parent / "driver-evidence.txt").write_text(out + err, encoding="utf-8")
    print(out[out.find('TABLE_GATE'):])
    print('exit_status=' + str(status))
    sys.exit(status)
finally:
    client.close()
