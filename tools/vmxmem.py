"""Read SVGA device state out of a live vmware-vmx.exe process, from the host.

Why this exists: every conclusion about the DX manager so far came either from
static disassembly (which just contradicted itself: the create block at 0x140509fe8
looks unreachable, yet a Windows guest on this very binary logs "Grow OTable" and
"DX10 context is on") or from x64dbg attach/detach cycles that break (§20.5). Both
are expensive. vmware-vmx runs as the same user, so PROCESS_VM_READ is enough to
just... look. Non-invasive, repeatable, scriptable.

The addresses are VAs under the default image base 0x140000000; the real VA is
    base + (va - 0x140000000)
where base is the first module in the process (the exe itself).

Usage:
    python vmxmem.py list
    python vmxmem.py <pid>
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

IB = 0x140000000
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModules.restype = wt.BOOL
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE),
                                     wt.DWORD, ctypes.POINTER(wt.DWORD)]
psapi.GetModuleBaseNameW.restype = wt.DWORD
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]

# Globals discovered this session (§48). Values are VAs under the default image base.
GLOBALS = [
    ("dev",         0x1413F31C0),  # [dev] = SVGA device object (NOT 0x1414931C0: 0 refs)
    ("dxManager",   0x1414930A0),  # the pointer DEFINE_RTV gates on
    ("svgaEnabled", 0x14140B043),  # byte, config init = 1, gate (C)
    ("svgaPhase",   0x14140B030),  # dword, init = 2
    ("fenceAgeUs",  0x14140B038),  # qword from svga.maxFenceAgeUS
    ("tkCounter1",  0x14140B068),  # compared against [block+0x40+idx*0x8028]
    ("tkCounter2",  0x141413090),  # same, init = 1
    ("tkLimitA",    0x14141B0B0),  # init = 0
    ("tkLimitB",    0x14141B0B8),  # init = 0
]
devFields = [
    (0xB4,  4, "b4   SVGA_REG_ENABLE shadow  <- gate: must be != 0"),
    (0x71,  1, "71   stopped flag            <- gate: must be 0"),
    (0x81D, 1, "81d  ENABLE bit0 / ctxs up    <- gate: must be != 0"),
    (0x81C, 1, "81c  reset-in-progress"),
    (0x9BC, 4, "9bc  any DC slot active?      <- gate: must be != 0"),
    (0xAD8, 4, "ad8  slot0 count"),
    (0xAE0, 2, "ae0  slot0 byte"),
    (0xBF0, 4, "bf0  slot1 count"),
    (0xBF8, 2, "bf8  slot1 byte"),
    (0xF4,  4, "f4   caps dword (bit28 = DX ctx)"),
    (0x1C0, 4, "1c0  caps2"),
    (0x19C, 4, "19c  cap word area"),
]


def list_vmx():
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    TH32CS_SNAPPROCESS = 0x2

    class PE32(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                    ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                    ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260)]

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PE32()
    e.dwSize = ctypes.sizeof(PE32)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    out = []
    while ok:
        if e.szExeFile.lower() == "vmware-vmx.exe":
            out.append(e.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def enable_debug_privilege():
    """Turn on SeDebugPrivilege for this process.

    Being an administrator is NOT enough here: vmware-vmx is started by the
    vmware-authd service at a higher privilege than the interactive user, so
    OpenProcess returns 5 even from an elevated shell until SeDebugPrivilege is
    actually enabled (it is present-but-disabled by default, per UAC).
    """
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    # Prototypes matter: without restype=HANDLE, GetCurrentProcess() returns a
    # 32-bit int and OpenProcessToken answers ERROR_INVALID_HANDLE (6) on x64.
    kernel.GetCurrentProcess.restype = wt.HANDLE
    advapi.OpenProcessToken.restype = wt.BOOL
    advapi.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
    advapi.LookupPrivilegeValueW.restype = wt.BOOL
    advapi.LookupPrivilegeValueW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p]
    advapi.AdjustTokenPrivileges.restype = wt.BOOL
    advapi.AdjustTokenPrivileges.argtypes = [wt.HANDLE, wt.BOOL, ctypes.c_void_p,
                                             wt.DWORD, ctypes.c_void_p, ctypes.c_void_p]

    class LUID(ctypes.Structure):
        _fields_ = [("LowPart", wt.DWORD), ("HighPart", wt.LONG)]

    class LUID_AND_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("Luid", LUID), ("Attributes", wt.DWORD)]

    class TOKEN_PRIVILEGES(ctypes.Structure):
        _fields_ = [("PrivilegeCount", wt.DWORD), ("Privileges", LUID_AND_ATTRIBUTES * 1)]

    TOKEN_QUERY = 0x0008
    TOKEN_ADJUST_PRIVILEGES = 0x0020
    SE_PRIVILEGE_ENABLED = 0x0002

    htok = wt.HANDLE()
    if not advapi.OpenProcessToken(kernel.GetCurrentProcess(),
                                   TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                                   ctypes.byref(htok)):
        return False, f"OpenProcessToken failed: {ctypes.get_last_error()}"
    luid = LUID()
    if not advapi.LookupPrivilegeValueW(None, "SeDebugPrivilege", ctypes.byref(luid)):
        return False, f"LookupPrivilegeValue failed: {ctypes.get_last_error()}"
    tp = TOKEN_PRIVILEGES()
    tp.PrivilegeCount = 1
    tp.Privileges[0].Luid = luid
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
    if not advapi.AdjustTokenPrivileges(htok, False, ctypes.byref(tp),
                                        ctypes.sizeof(tp), None, None):
        return False, f"AdjustTokenPrivileges failed: {ctypes.get_last_error()}"
    err = ctypes.get_last_error()
    if err == 1300:  # ERROR_NOT_ALL_ASSIGNED -- token does not have the privilege
        return False, "SeDebugPrivilege not held by this token (need admin)"
    kernel.CloseHandle(htok)
    return True, "ok"


def open_proc(pid):
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise OSError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")
    return h


def read(h, va, n):
    buf = (ctypes.c_char * n)()
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(va), buf, n, ctypes.byref(got)):
        raise OSError(f"ReadProcessMemory({va:#x}) failed: {ctypes.get_last_error()}")
    return buf.raw


def module_base(h):
    mods = (wt.HMODULE * 64)()
    need = wt.DWORD(0)
    if not psapi.EnumProcessModules(h, mods, ctypes.sizeof(mods), ctypes.byref(need)):
        raise OSError(f"EnumProcessModules failed: {ctypes.get_last_error()}")
    return mods[0]


def snapshot(pid, out):
    h = open_proc(pid)
    base = module_base(h)
    out.append(f"pid {pid}  module base {base:#x}")
    got = {}
    for name, va in GLOBALS:
        raw = read(h, base + (va - IB), 8)
        val = struct.unpack("<Q", raw)[0]
        got[name] = val
        out.append(f"  [{name:12s}] @ {va:#x} = {val:#016x}")
    dev = got["dev"]
    if not dev:
        out.append("  dev is NULL -- nothing else to read")
        return
    out.append(f"--- dev = {dev:#x}")
    for off, size, label in devFields:
        raw = read(h, dev + off, size)
        v = struct.unpack({1: "<B", 2: "<H", 4: "<I"}[size], raw)[0]
        out.append(f"    +{off:#06x} = {v:#010x} ({v})   {label}")
    dm = got["dxManager"]
    out.append(f"--- DX manager [0x1414930A0] = {dm:#x}"
               + ("   <-- CREATED" if dm else "   <-- NULL (DEFINE_RTV will fail)"))


WATCH_FIELDS = [
    ("dxManager", 0x1414930A0, 8),
    ("9bc",       None, 4),
    ("ad8",       None, 4),
    ("bf0",       None, 4),
    ("ae0",       None, 2),
    ("bf8",       None, 2),
]


def watch(pid, seconds, out):
    """Sample the interesting fields continuously.

    A single sample cannot tell "never created" from "created on demand and already
    destroyed" -- and the Windows guest on this host runs DX fine while a one-shot
    read says NULL. Repeated sampling distinguishes them: a flickering non-NULL
    means on-demand; a constant NULL means the pointer is simply not on this path.
    """
    import time
    h = open_proc(pid)
    base = module_base(h)
    dev = struct.unpack("<Q", read(h, base + (0x1413F31C0 - IB), 8))[0]
    out.append(f"pid {pid} watching {seconds}s  dev={dev:#x}")
    seen = {}
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds:
        for name, va, size in WATCH_FIELDS:
            addr = (base + (va - IB)) if va else (dev + int(name, 16))
            try:
                raw = read(h, addr, size)
            except OSError:
                continue
            v = int.from_bytes(raw, "little")
            key = name
            seen.setdefault(key, {}).setdefault(v, 0)
            seen[key][v] += 1
        n += 1
        time.sleep(0.05)
    out.append(f"  {n} samples")
    for name, _, _ in WATCH_FIELDS:
        if name not in seen:
            continue
        vals = sorted(seen[name].items(), key=lambda kv: -kv[1])
        txt = "  ".join(f"{v:#x}x{c}" for v, c in vals[:6])
        out.append(f"    {name:10s}: {txt}")


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "list":
        for pid in list_vmx():
            print(pid)
        return 0
    out_path = None
    if "--out" in argv:
        i = argv.index("--out")
        out_path = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    secs = 0
    if "--watch" in argv:
        i = argv.index("--watch")
        secs = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]
    if argv[0] == "all":
        pids = list_vmx()
    else:
        pids = [int(argv[0])]
    out = []
    ok, why = enable_debug_privilege()
    out.append(f"SeDebugPrivilege: {why}")
    for pid in pids:
        try:
            if secs:
                watch(pid, secs, out)
            else:
                snapshot(pid, out)
        except OSError as exc:
            out.append(f"pid {pid}: {exc}  (needs elevation: vmx runs at high integrity)")
    text = "\n".join(out)
    print(text)
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
