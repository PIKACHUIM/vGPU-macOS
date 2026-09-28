#!/usr/bin/env python3
"""Set or remove keys in a VMware `.vmx` without disturbing anything else.

VMware rewrites the whole file on every power cycle, reordering and re-adding keys,
so hand-editing is fragile. This tool keeps the original line order and comments and
only touches the keys you name.

The file declares its own codec in the first line (`.encoding = "GBK"` on this VM,
because of Chinese comments), so that is honoured rather than assuming UTF-8.

Usage:
    python tools/vmxset.py VM.vmx --set sata0:1.present=FALSE
    python tools/vmxset.py VM.vmx --set RemoteDisplay.vnc.enabled=TRUE --set RemoteDisplay.vnc.port=5901
    python tools/vmxset.py VM.vmx --unset RemoteDisplay.vnc.password
    python tools/vmxset.py VM.vmx --set guestOS=darwin24-64 --dry-run

Values are written verbatim inside double quotes unless they are already quoted.
Run against a powered-off VM; VMware overwrites the file on power off.

Always keeps a backup: writes ``<vmx>.<timestamp>.bak`` before modifying, and
``--no-backup`` turns that off. Restore by copying the backup back over.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import sys
import time

ENCODING_RE = re.compile(r'^\s*\.encoding\s*=\s*"([^"]+)"', re.IGNORECASE)
KEY_RE = re.compile(r'^\s*([A-Za-z0-9_.:\-]+)\s*=\s*(.*?)\s*$')


def detect_codec(payload: bytes) -> str:
    """Read `.encoding` from the raw bytes; VMware writes the file in that codec."""
    head = payload[:4096].decode("latin-1", "replace")
    match = ENCODING_RE.search(head)
    if match:
        name = match.group(1).strip()
        try:
            "".encode(name)
            return name
        except LookupError:
            pass
    return "utf-8"


def quote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value
    return f'"{value}"'


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vmx", help="path to the .vmx file")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                    help="set or append a key (repeatable)")
    ap.add_argument("--unset", action="append", default=[], metavar="KEY",
                    help="comment out a key (repeatable)")
    ap.add_argument("--dry-run", action="store_true", help="show the diff, write nothing")
    ap.add_argument("--no-backup", action="store_true", help="skip the .bak copy")
    args = ap.parse_args(argv)

    path = pathlib.Path(args.vmx)
    if not path.is_file():
        sys.exit(f"not a file: {path}")

    raw = path.read_bytes()
    codec = detect_codec(raw)
    text = raw.decode(codec, "surrogateescape")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)

    wanted: dict[str, str] = {}
    for item in args.set:
        if "=" not in item:
            sys.exit(f"--set expects KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        wanted[key.strip()] = value.strip()
    unwanted = {k.strip() for k in args.unset}

    changes: list[str] = []
    seen: set[str] = set()

    for i, line in enumerate(lines):
        match = KEY_RE.match(line)
        if not match or line.lstrip().startswith("#"):
            continue
        key = match.group(1)
        if key in wanted:
            old = match.group(2)
            new = quote(wanted[key])
            if old == new:
                seen.add(key)
                continue
            lines[i] = f"{key} = {new}"
            changes.append(f"  {key}: {old} -> {new}")
            seen.add(key)
        elif key in unwanted:
            lines[i] = f"# {line}   # disabled by vmxset {time.strftime('%Y-%m-%d')}"
            changes.append(f"  {key}: commented out")

    # Append keys that were not present at all, before any trailing blank lines.
    missing = [k for k in wanted if k not in seen]
    if missing:
        insert_at = len(lines)
        while insert_at > 0 and not lines[insert_at - 1].strip():
            insert_at -= 1
        additions = [f"{k} = {quote(wanted[k])}" for k in missing]
        lines[insert_at:insert_at] = additions
        for k, line in zip(missing, additions):
            changes.append(f"  {k}: (absent) -> {line.split(' = ', 1)[1]}")

    for key in unwanted:
        if key not in {m.group(1) for m in map(KEY_RE.match, lines) if m}:
            continue

    if not changes:
        print("no changes")
        return 0

    print(f"{path.name}: {len(changes)} change(s)")
    print("\n".join(changes))

    if args.dry_run:
        print("dry run: nothing written")
        return 0

    if not args.no_backup:
        backup = path.with_suffix(path.suffix + f".{time.strftime('%Y%m%d-%H%M%S')}.bak")
        shutil.copy2(path, backup)
        print(f"backup: {backup}")

    path.write_bytes(newline.join(lines).encode(codec, "surrogateescape"))
    print(f"wrote {path} ({codec})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
