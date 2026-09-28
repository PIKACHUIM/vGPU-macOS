#!/usr/bin/env python3
"""Build a minimal OpenCore tree for the vGPU-macOS test guest.

Starts from OpenCore's own ``Docs/Sample.plist`` so every key exists with a sane value, then
changes only what this project needs and prints a summary of the differences. Anything the
sample ships that would break a headless, non-Apple-secure-boot VMware guest is forced off:

  * ``Misc -> Security -> Vault``           Secure -> Optional
        With Vault=Secure, OpenCore requires ``EFI/OC/vault.plist`` and ``vault.sig`` and
        simply refuses to boot without them. This is the single most common way a first
        OpenCore attempt ends in a black screen.
  * ``Misc -> Security -> SecureBootModel``  Default -> Disabled
        There is no Apple secure boot on this guest, and the model picks an Apple-specific
        DMG/APFS policy we do not want.
  * ``Misc -> Boot -> ShowPicker`` / Timeout  -> False / 0
        There is no way to press a key at the console. A picker would hang the boot.
  * ``NVRAM -> Add``                         emptied
        The sample seeds ``csr-active-config`` and other variables; we already set those
        deliberately, and letting OpenCore rewrite them would undo that.
  * ``Kernel -> Add`` / ``UEFI -> Drivers``  replaced with just what this project needs.

Usage:
    python tools/oc-build.py [--release|--debug] [--out .oc/build]
"""

from __future__ import annotations

import argparse
import pathlib
import plistlib
import shutil
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
KEXT_SRC = REPO_ROOT / "kexts" / "framebuffer" / "build" / "vgpuFramebuffer.kext"
KEXT_NAME = "vgpuFramebuffer.kext"
KEXT_BIN = "vgpuFramebuffer"

# Drivers this project needs and nothing else. OpenRuntime is mandatory for Kernel -> Add
# (it installs the runtime services OpenCore needs after ExitBootServices); OpenPartitionDxe
# is the partition scanner that makes APFS containers visible. Everything the sample also
# lists -- Virtio*, HfsPlus, OpenCanopy, AudioDxe -- is dead weight here.
DRIVERS = [
    ("OpenRuntime.efi", "required for Kernel->Add and memory fixes"),
    ("OpenPartitionDxe.efi", "partition scanning"),
]

# Misc -> Debug -> Target is a bitmask: 1 logging, 2 console, 4 datahub, 8 serial,
# 16 variable, 32 file, 64 custom. 127 turns everything on, which matters here because the
# only way to read the result is the log file it writes back to the ESP.
DEBUG_TARGET = 127


def build(variant: str, out_dir: pathlib.Path) -> int:
    oc_root = REPO_ROOT / ".oc"
    src_root = oc_root / ("oc" if variant == "release" else "ocdbg")
    sample = oc_root / "oc" / "Docs" / "Sample.plist"          # sample is shared
    if not sample.is_file():
        sys.exit(f"missing {sample}; run the download step in docs/opencore.md first")
    if not (src_root / "X64" / "EFI" / "OC" / "OpenCore.efi").is_file():
        sys.exit(f"missing {src_root}/X64/EFI/OC/OpenCore.efi")
    if not KEXT_SRC.is_dir():
        sys.exit(f"missing {KEXT_SRC}; build it with kexts/framebuffer/build.sh on the guest")

    plist = plistlib.loads(sample.read_bytes())
    changes: list[str] = []

    def set_key(path: list[str], value) -> None:
        node = plist
        for key in path[:-1]:
            node = node[key]
        old = node.get(path[-1], "<absent>")
        if old != value:
            changes.append(f"  {' -> '.join(path)}: {old!r} -> {value!r}")
        node[path[-1]] = value

    # --- what to inject -------------------------------------------------------------
    plist["Kernel"]["Add"] = [
        {
            "Arch": "x86_64",
            "BundlePath": KEXT_NAME,
            "Comment": "vGPU-macOS: minimal IOFramebuffer subclass",
            "Enabled": True,
            "ExecutablePath": f"Contents/MacOS/{KEXT_BIN}",
            "MaxKernel": "",
            "MinKernel": "",
            "PlistPath": "Contents/Info.plist",
        }
    ]
    changes.append(f"  Kernel -> Add: replaced with {KEXT_NAME} only")

    plist["Kernel"]["Block"] = []
    plist["Kernel"]["Force"] = []
    plist["Kernel"]["Patch"] = []
    changes.append("  Kernel -> Block/Force/Patch: emptied")

    # --- drivers --------------------------------------------------------------------
    plist["UEFI"]["Drivers"] = [
        {"Comment": comment, "Enabled": True, "Path": path} for path, comment in DRIVERS
    ]
    changes.append(f"  UEFI -> Drivers: replaced with {[d for d, _ in DRIVERS]}")

    # --- the three settings that stop a headless first boot -------------------------
    set_key(["Misc", "Security", "Vault"], "Optional")
    set_key(["Misc", "Security", "SecureBootModel"], "Disabled")
    set_key(["Misc", "Boot", "ShowPicker"], False)
    set_key(["Misc", "Boot", "Timeout"], 0)
    set_key(["Misc", "Boot", "TakeoffDelay"], 0)

    # --- do not let OpenCore rewrite NVRAM ------------------------------------------
    plist["NVRAM"]["Add"] = {}
    plist["NVRAM"]["Delete"] = {}
    changes.append("  NVRAM -> Add/Delete: emptied (csr-active-config is set deliberately)")

    # --- logging --------------------------------------------------------------------
    set_key(["Misc", "Debug", "Target"], DEBUG_TARGET)
    set_key(["Misc", "Debug", "LogModules"], ["*"])
    set_key(["Misc", "Debug", "DisplayDelay"], 0)

    # --- assemble the tree ----------------------------------------------------------
    efi = out_dir / "EFI"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    (efi / "OC" / "Drivers").mkdir(parents=True)
    (efi / "OC" / "Kexts").mkdir()
    (efi / "BOOT").mkdir()

    shutil.copy2(src_root / "X64" / "EFI" / "BOOT" / "BOOTx64.efi", efi / "BOOT" / "BOOTx64.efi")
    shutil.copy2(src_root / "X64" / "EFI" / "OC" / "OpenCore.efi", efi / "OC" / "OpenCore.efi")
    for path, _ in DRIVERS:
        shutil.copy2(src_root / "X64" / "EFI" / "OC" / "Drivers" / path, efi / "OC" / "Drivers" / path)
    shutil.copytree(KEXT_SRC, efi / "OC" / "Kexts" / KEXT_NAME)
    (efi / "OC" / "config.plist").write_bytes(plistlib.dumps(plist))

    print(f"variant: {variant}")
    print(f"tree:    {out_dir}")
    print("changes against Sample.plist:")
    for line in changes:
        print(line)
    print("\ncontents:")
    for p in sorted(efi.rglob("*")):
        if p.is_file():
            print(f"  {p.relative_to(out_dir)}  ({p.stat().st_size} bytes)")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--debug", action="store_true", help="use the DEBUG build (logs much more)")
    ap.add_argument("--release", action="store_true", help="use the RELEASE build")
    ap.add_argument("--out", default=str(REPO_ROOT / ".oc" / "build"))
    args = ap.parse_args(argv)
    if args.debug and args.release:
        sys.exit("pick one of --debug / --release")
    return build("debug" if args.debug else "release", pathlib.Path(args.out))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
