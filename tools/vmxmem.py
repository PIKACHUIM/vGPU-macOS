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
    python vmxmem.py <pid> --watch 20
    python vmxmem.py <pid> --poke 0x1414937FC=1     # runtime gate flip (§60.4)
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

IB = 0x140000000
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_OPERATION = 0x0008
PAGE_READWRITE = 0x04

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
    # 11-16 §60: this is NOT a "DX manager".  0x140a17350 is a hash PROBE
    # (bucket = hash(id) & mask, count from 0x141492ED0, buckets at 0x1414930B0),
    # and the only store is  `find_or_create(cbHeader.dxContext) -> [0x1414930A0]`
    # inside the CB-header DX branch.  So it holds the CURRENT DX CONTEXT and is
    # written only when a command buffer arrives with SVGA_CB_FLAG_DX_CONTEXT.
    ("dxCtx",       0x1414930A0),  # current DX context object (NULL = no ctx bound)
    # 0x14050a058: `cmp [0x1414937FC], 0 ; je -> status 4`.  Across all of .text
    # there are 49 references and ZERO writes -- it can never become non-zero, so
    # the DX branch of the CB header processor is unreachable.  Reading 0 here
    # reproduces the STATUS 4 that every DX-flagged submission has returned.
    ("dxGate",      0x1414937FC),  # dword; != 0 required for the DX branch
    ("dxBuckets",   0x1414930B0),  # hash table bucket array (dxCtx lookup)
    ("dxHashCnt",   0x141492ED0),  # hash table bucket count (/mask)
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


def open_proc(pid, write=False):
    access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    if write:
        access |= PROCESS_VM_WRITE | PROCESS_VM_OPERATION
    h = k32.OpenProcess(access, False, pid)
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
    dm = got["dxCtx"]
    out.append(f"--- current DX context [0x1414930A0] = {dm:#x}"
               + ("   <-- a DX context object exists" if dm else
                  "   <-- NULL: no CB ever took the DX_CONTEXT branch"))
    gate = got["dxGate"] & 0xFFFFFFFF
    out.append(f"--- DX header gate  [0x1414937FC] = {gate:#x}"
               + ("   <-- DX branch reachable" if gate else
                  "   <-- ZERO: every DX-flagged CB is answered STATUS 4"))
    out.append(f"--- dx hash buckets  [0x1414930B0] = {got['dxBuckets']:#x}   "
               f"count [0x141492ED0] = {got['dxHashCnt'] & 0xFFFFFFFF:#x}")


WATCH_FIELDS = [
    ("dxCtx",    0x1414930A0, 8),
    ("dxGate",   0x1414937FC, 4),
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


def poke(pid, pokes, out):
    """Write dwords into the running vmx (experiment: flip a feature gate).

    §60.4 established that [0x1414937FC] has no writer anywhere in .text, so the
    DX branch of the command-buffer header processor looks unreachable.  Poking it
    to 1 at runtime is the cheapest way to find out whether it is the *only*
    obstacle: if the next DX-flagged submission gets past the gate, [0x1414930A0]
    must turn non-NULL -- an effect no other experiment has ever produced.
    """
    h = open_proc(pid, write=True)
    base = module_base(h)
    for va, size, val in pokes:
        addr = base + (va - IB)
        old = wt.DWORD(0)
        k32.VirtualProtectEx(ctypes.c_void_p(addr), ctypes.c_size_t(size),
                             PAGE_READWRITE, ctypes.byref(old))
        before = int.from_bytes(read(h, addr, size), "little")
        buf = val.to_bytes(size, "little")
        wrote = ctypes.c_size_t(0)
        ok = k32.WriteProcessMemory(h, ctypes.c_void_p(addr), buf, size,
                                    ctypes.byref(wrote))
        after = int.from_bytes(read(h, addr, size), "little")
        out.append(f"  poke [{va:#x}] {size}B: {before:#x} -> {after:#x}"
                   + ("   ok" if (ok and after == val)
                      else f"   FAILED err={ctypes.get_last_error()}"))
    gate = int.from_bytes(read(h, base + (0x1414937FC - IB), 4), "little")
    ctx = int.from_bytes(read(h, base + (0x1414930A0 - IB), 8), "little")
    out.append(f"--- after poke: dxGate = {gate:#x}   dxCtx = {ctx:#x}"
               + ("   <-- DX branch is now REACHABLE" if gate else ""))


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
    pokes = []
    while "--poke" in argv:
        i = argv.index("--poke")
        spec = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
        lhs, rhs = spec.split("=")
        pokes.append((int(lhs, 16), 4, int(rhs, 0)))
    if argv[0] == "all":
        pids = list_vmx()
    else:
        pids = [int(argv[0])]
    out = []
    ok, why = enable_debug_privilege()
    out.append(f"SeDebugPrivilege: {why}")
    for pid in pids:
        try:
            if pokes:
                poke(pid, pokes, out)
            elif secs:
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
