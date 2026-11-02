#!/usr/bin/env python3
"""Run one probe experiment end to end: build, install, reboot, collect the log.

Every experiment on this guest costs exactly one reboot because macOS 15 refuses to
load a *changed* AuxKC kext without one (KMErrorDomain Code=28, on `kmutil load -z`,
`kextutil`, and `kmutil load --bundle-path` alike). Automating the reboot is therefore
the difference between a five-minute iteration and a fifteen-minute one.

Usage:
    python tools/probe-cycle.py 0                    # read-only snapshot
    python tools/probe-cycle.py 1 1920 1080          # write WIDTH/HEIGHT
    python tools/probe-cycle.py 2 1920 1080          # write MAX_WIDTH/MAX_HEIGHT

The log is filtered down to the `vgpu-probe:` lines, which is all this kext emits.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
MSSH = REPO_ROOT / "tools" / "mssh.py"
VENV_PY = pathlib.Path(r"C:\Users\Pika\WorkBuddy\2026-09-27-20-30-43\.venv\Scripts\python.exe")
REMOTE = "/var/tmp/vgpu-probe"


def run(remote_cmd: str, sudo: bool = False, timeout: int = 180) -> str:
    py = VENV_PY if VENV_PY.is_file() else pathlib.Path(sys.executable)
    args = [str(py), str(MSSH)]
    args.append("--sudo" if sudo else "--")
    args.append(remote_cmd)
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return proc.stdout + proc.stderr


def upload(local: pathlib.Path) -> None:
    py = VENV_PY if VENV_PY.is_file() else pathlib.Path(sys.executable)
    subprocess.run(
        [str(py), str(MSSH), "--put", str(local), f"{REMOTE}/{local.name}"],
        check=True, capture_output=True, text=True, timeout=120,
    )


def wait_for_ssh(deadline_s: int = 300) -> bool:
    """Poll until the guest answers, but not by watching for a reboot it never did.

    `shutdown -r now` returns before the machine drops, so the first probe succeeds
    against the *old* instance. Sleep past the expected drop window before trusting
    any successful connection.
    """
    time.sleep(40)
    started = time.time()
    while time.time() - started < deadline_s:
        if "up" in run("uptime", timeout=30):
            return True
        time.sleep(8)
    return False


def _since_boot(raw: str, boot_epoch: int) -> list:
    """Keep only log lines stamped at or after the boot, and strip the prefix.

    `log show --style compact` stamps each line as `YYYY-MM-DD HH:MM:SS.mmm`, but omits the
    date when it matches the previous line's -- so a run that begins the day before a boot
    can emit bare `HH:MM:SS.mmm`. Rather than parse around that, seed the date from the
    boot time and only accept lines whose clock time is at or after the boot's, which is
    the only case that matters here: everything before the boot is being discarded anyway.

    Comparison is on the string prefix, which sorts correctly for ISO-style timestamps.
    """
    boot_dt = time.localtime(boot_epoch)
    boot_stamp = time.strftime("%Y-%m-%d %H:%M:%S", boot_dt)
    boot_hm = time.strftime("%H:%M:%S", boot_dt)

    kept = []
    for line in raw.splitlines():
        if "vgpu-probe: " not in line:
            continue
        body = line.split("vgpu-probe: ", 1)[1]
        # The stamp is the first two whitespace-separated fields of the compaction.
        head = line[:40]
        m_full = re.search(r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", head)
        m_time = re.search(r"(\d{2}:\d{2}:\d{2})", head)
        if m_full:
            if m_full.group(1) >= boot_stamp:
                kept.append(body)
        elif m_time:
            if m_time.group(1) >= boot_hm:
                kept.append(body)
        # A line with no recognisable stamp is dropped rather than guessed at: the whole
        # point of this filter is that "which boot produced this" must not be ambiguous.
    return kept


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", type=int, help="0=read-only, 1=write WIDTH/HEIGHT, 2=write MAX")
    ap.add_argument("width", type=int, nargs="?", default=0)
    ap.add_argument("height", type=int, nargs="?", default=0)
    ap.add_argument("--no-reboot", action="store_true", help="only rebuild and install")
    args = ap.parse_args()

    print(f"=== experiment mode={args.mode} {args.width}x{args.height} ===")

    for name in ("vgpuProbe.cpp", "buildx.sh", "Info.plist"):
        upload(REPO_ROOT / "kexts" / "probe" / name)

    out = run(f"cd {REMOTE} && sh buildx.sh {args.mode} {args.width} {args.height} 2>&1", timeout=300)
    if "OK: _kmod_info present" not in out:
        print("BUILD FAILED")
        print("\n".join(l for l in out.splitlines() if "error" in l.lower())[:3000])
        return 1
    print("build ok")

    # Replace the bundle in place, then stage it. The staging record is what survives the
    # reboot; dropping the stale AuxKC first avoids a build whose contents do not match.
    stage = (
        f"rm -rf /Library/Extensions/vgpuProbe.kext && "
        f"cp -R {REMOTE}/build/vgpuProbe.kext /Library/Extensions/ && "
        f"chown -R 0:0 /Library/Extensions/vgpuProbe.kext && "
        f"chmod -R 755 /Library/Extensions/vgpuProbe.kext && "
        f"kmutil clear-staging >/dev/null 2>&1; "
        f"kmutil load -z -p /Library/Extensions/vgpuProbe.kext 2>&1 | head -2"
    )
    print(run(stage, sudo=True).strip())

    if args.no_reboot:
        return 0

    print("rebooting...")
    run("shutdown -r now", sudo=True, timeout=60)

    if not wait_for_ssh():
        print("guest did not come back")
        return 1
    print("ssh up")

    # Bound the query by the actual boot, not by a fixed window. `--last 6m` looks safe
    # because the reboot we just did is recent, but the guest comes back in well under a
    # minute, so a 6-minute window reaches back across the reboot and mixes the previous
    # experiment's snapshot into this one's output -- which is exactly how a stale
    # "INCONSISTENT" line from a superseded build ended up looking like a live regression.
    # `--last` keeps the query bounded (an unbounded one times out over SSH on a fresh
    # boot); the timestamp filter drops anything logged before this boot.
    boot = run("sysctl -n kern.boottime", timeout=30)
    m = re.search(r"sec = (\d+)", boot)
    if not m:
        print(f"could not read boot time: {boot!r}")
        return 1
    boot_epoch = int(m.group(1))

    raw = run(
        'sudo log show --last 10m --style compact 2>/dev/null | grep "vgpu-probe:"',
        timeout=300,
    )
    lines = _since_boot(raw, boot_epoch)
    print("--- probe log (this boot only) ---")
    print("\n".join(lines) or "(nothing logged)")

    loaded = run("kextstat 2>/dev/null | grep -i vgpu || echo '(not loaded)'", timeout=60)
    print("--- kextstat ---")
    print(loaded.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
