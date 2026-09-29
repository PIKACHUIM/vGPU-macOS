#!/usr/bin/env python3
"""Add a kext to kernelmanagerd's auxiliary-collection build instructions.

Why this exists
---------------
An IOFramebuffer subclass cannot be hot-loaded: `kmutil load` refuses it with
Code=28 "requires a reboot" because it depends on IOGraphicsFamily, which lives
in the *pageable* System KC, and a pageable dependency cannot be resolved for a
kext that is not itself in a pageable collection. So the kext has to be built
into the auxiliary kernel collection, and only kernelmanagerd may do that.

kernelmanagerd rebuilds the auxiliary collection from two files:

  AuxKC/CurrentAuxKC/com.apple.kcgen.instructions.plist
      kextsToBuild -- which bundles go into the new collection

  AuxKC/CurrentAuxKC/com.apple.kcgen.uakl.plist
      SignedKernelExtensions   -- teamID -> [(bundle id, cdhash), ...]
      UnsignedKernelExtensions -- [cdhash, ...]

The second file is the loadable list: after the kernel loads the collection,
kernelmanagerd logs "AuxKC bundle <id> marked as loadable" for each entry, and
only those are actually started. A bundle that is present in the collection but
absent from the UAKL is silently never loaded.

Normally both files are produced from a `kmutil load` request that the user
approves in System Settings. On this guest that path does not complete:
syspolicyd refuses with "Unsigned kext not present in the legacy kext list"
because the kext has no team ID, so kernelmanagerd never adds it to
kextsToBuild -- even though its cdhash does end up in UnsignedKernelExtensions.

This script edits kextsToBuild directly. It is a diagnostic tool for a
throwaway VM with SIP disabled, not something to ship.

Usage (as root, on the guest):
  python3 guest-patch-kcinstructions.py --show
  python3 guest-patch-kcinstructions.py --bundle-id com.example.x \\
      --path /Library/Extensions/x.kext --cdhash <40 hex chars>
  python3 guest-patch-kcinstructions.py --remove --bundle-id com.example.x
"""

import argparse
import pathlib
import plistlib
import shutil
import sys

DEFAULT_PLIST = (
    "/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/"
    "com.apple.kcgen.instructions.plist"
)


def dump(kexts):
    print("kextsToBuild now:")
    for name in sorted(kexts):
        entry = kexts[name]
        print(
            "  {}\n      path   {}\n      cdhash {}\n      teamID {!r}".format(
                name,
                entry.get("bundlePathMainOS"),
                entry.get("cdHash"),
                entry.get("teamID"),
            )
        )


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--plist", default=DEFAULT_PLIST)
    ap.add_argument("--bundle-id", help="bundle identifier to add or remove")
    ap.add_argument("--path", help="bundle path on the main OS")
    ap.add_argument("--cdhash", help="40 hex character cdhash")
    ap.add_argument("--team-id", default="", help="team ID (empty is valid here)")
    ap.add_argument("--remove", action="store_true", help="remove instead of add")
    ap.add_argument("--show", action="store_true", help="print and exit")
    ap.add_argument("--no-backup", action="store_true")
    args = ap.parse_args(argv)

    path = pathlib.Path(args.plist)
    if not path.exists():
        return f"no such plist: {path}"

    doc = plistlib.loads(path.read_bytes())
    kexts = doc.setdefault("kextsToBuild", {})

    if args.show:
        dump(kexts)
        return 0

    if not args.bundle_id:
        return "--bundle-id is required unless --show is given"

    if args.remove:
        kexts.pop(args.bundle_id, None)
        print("removed {}".format(args.bundle_id))
    else:
        if not args.path or not args.cdhash:
            return "adding requires --path and --cdhash"
        if len(args.cdhash) != 40:
            return "cdhash should be 40 hex characters, got {} ".format(
                len(args.cdhash)
            )
        kexts[args.bundle_id] = {
            "bundlePathMainOS": args.path,
            "cdHash": args.cdhash,
            "teamID": args.team_id,
        }
        print("set {} -> {}".format(args.bundle_id, args.path))

    if not args.no_backup:
        backup = path.with_name(path.name + ".vgpu-bak")
        if not backup.exists():
            shutil.copy2(path, backup)
            print("backup at {}".format(backup))

    # kernelmanagerd writes these as XML; keep that.
    path.write_bytes(plistlib.dumps(doc, fmt=plistlib.FMT_XML))
    dump(kexts)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
