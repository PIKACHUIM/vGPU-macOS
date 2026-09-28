#!/usr/bin/env python3
"""Dump the EFI variable store inside a VMware Workstation `.nvram` file.

VMware keeps an EDK2-style variable store in `<vm>.nvram`, magic ``MRVN``. The
records are packed back to back:

    u8   guid[16]      EFI mixed-endian (first three fields little-endian)
    u32  attributes    EFI_VARIABLE_* bits (NV=1, BS=2, RT=4)
    u32  total_len     name_len + value_len
    u32  name_len      byte count of the name, including its NUL terminator
    u8   name[name_len]   UTF-16LE, U+0000 terminated
    u8   value[total_len - name_len]

There is no per-record checksum, and no length prefix at the front of the file.

Usage:
    python nvramdump.py <file.nvram> [--list] [--find NAME] [--hex NAME]
    python nvramdump.py <file.nvram> --rebuild <out.nvram> <script.json>

The ``--rebuild`` mode rewrites a store from a JSON edit script so a variable
can be added or replaced offline (the VM must be powered off).
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
import uuid

APPLE_BOOT_GUID = "7c436110-ab2a-4bbb-a880-fe41995c9f82"
EFI_GLOBAL_GUID = "8be4df61-93ca-11d2-aa0d-00e098032b8c"


def guid_to_bytes(text: str) -> bytes:
    return uuid.UUID(text).bytes_le


def bytes_to_guid(raw: bytes) -> str:
    return str(uuid.UUID(bytes_le=raw))


def looks_like_record(buf: bytes, off: int) -> tuple[int, int, int] | None:
    """Return (attr, total, name_len) if a plausible record header sits at off."""
    if off + 28 > len(buf):
        return None
    attr, total, name_len = struct.unpack_from("<III", buf, off + 16)
    if total == 0 or total > 0x10000:
        return None
    if name_len < 4 or name_len > total or name_len % 2:
        return None
    if off + 28 + total > len(buf):
        return None
    raw_name = buf[off + 28 : off + 28 + name_len]
    if raw_name[-2:] != b"\x00\x00":
        return None
    body = raw_name[:-2]
    # Every other byte must be NUL, the rest printable ASCII.
    for i in range(0, len(body), 2):
        if body[i + 1] != 0 or not (0x20 <= body[i] < 0x7F):
            return None
    return attr, total, name_len


def walk(buf: bytes, start: int, limit: int = 100_000) -> list[dict]:
    """Walk packed records from start until the run breaks."""
    out: list[dict] = []
    off = start
    while off < len(buf) and len(out) < limit:
        head = looks_like_record(buf, off)
        if head is None:
            break
        attr, total, name_len = head
        name = buf[off + 28 : off + 28 + name_len - 2].decode("utf-16-le", "replace")
        value = buf[off + 28 + name_len : off + 28 + total]
        out.append(
            {
                "offset": off,
                "size": 28 + total,
                "guid": bytes_to_guid(buf[off : off + 16]),
                "attributes": attr,
                "name": name,
                "value": value,
            }
        )
        off += 28 + total
    return out


def find_store_start(buf: bytes) -> list[int]:
    """Candidate offsets where a packed record run begins."""
    # A record run always starts right after a GUID. Search for the two GUIDs
    # that macOS always registers, then rewind to the run head.
    starts: list[int] = []
    for probe in (guid_to_bytes(EFI_GLOBAL_GUID), guid_to_bytes(APPLE_BOOT_GUID)):
        off = 0
        while True:
            i = buf.find(probe, off)
            if i < 0:
                break
            # Rewind over contiguous records to find the beginning of the run.
            head = i
            while head >= 28:
                prev = None
                for back in range(28, min(0x4000, head + 1)):
                    if looks_like_record(buf, head - back) and (
                        head - back + 28 + looks_like_record(buf, head - back)[1] == head
                    ):
                        prev = head - back
                        break
                if prev is None:
                    break
                head = prev
            if head not in starts:
                starts.append(head)
            off = i + 1
    return sorted(starts)


def describe(value: bytes) -> str:
    """Best-effort printable rendering of a value."""
    if len(value) == 0:
        return "<empty>"
    # UTF-16LE string?
    if len(value) % 2 == 0 and value[-2:] == b"\x00\x00":
        body = value[:-2]
        if body and all(body[i + 1] == 0 and 0x20 <= body[i] < 0x7F for i in range(0, len(body), 2)):
            return repr(body.decode("utf-16-le"))
    if all(0x20 <= b < 0x7F for b in value):
        return repr(value.decode("ascii"))
    if len(value) == 4:
        (n,) = struct.unpack("<I", value)
        return f"u32 {n} (0x{n:x})"
    if len(value) == 1:
        return f"u8 {value[0]} (0x{value[0]:x})"
    return value.hex()


def cmd_list(buf: bytes) -> int:
    starts = find_store_start(buf)
    print(f"file size {len(buf)} bytes; candidate store starts: {starts}")
    for start in starts:
        recs = walk(buf, start)
        if not recs:
            print(f"  @{start}: no records")
            continue
        end = recs[-1]["offset"] + recs[-1]["size"]
        print(f"\n=== store @{start} .. {end}  ({len(recs)} records, {end - start} bytes) ===")
        print(f"{'offset':>8} {'size':>6}  {'attr':>4}  {'name':36s} value")
        for r in recs:
            print(
                f"{r['offset']:8d} {r['size']:6d}  {r['attributes']:4d}  "
                f"{r['name']:36s} {describe(r['value'])}"
            )
        tail = buf[end : end + 32]
        if tail:
            print(f"  first bytes after store: {tail.hex()}")
    return 0


def cmd_find(buf: bytes, name: str, as_hex: bool) -> int:
    starts = find_store_start(buf)
    hits = 0
    for start in starts:
        for r in walk(buf, start):
            if r["name"] == name:
                hits += 1
                print(
                    f"@{r['offset']} size={r['size']} attr={r['attributes']} "
                    f"guid={r['guid']}"
                )
                print(f"  raw value ({len(r['value'])} B): {r['value'].hex()}")
                if not as_hex:
                    print(f"  decoded: {describe(r['value'])}")
    if not hits:
        print(f"{name!r}: not present")
    return 0 if hits else 1


def encode_record(guid: str, attributes: int, name: str, value: bytes) -> bytes:
    name_raw = name.encode("utf-16-le") + b"\x00\x00"
    total = len(name_raw) + len(value)
    return (
        guid_to_bytes(guid)
        + struct.pack("<III", attributes, total, len(name_raw))
        + name_raw
        + value
    )


def cmd_rebuild(buf: bytes, out_path: str, script_path: str) -> int:
    """Apply a JSON edit script to a copy of the store.

    Script format:
        {"sets": [{"name": "csr-active-config", "guid": "7c436110-...",
                   "attributes": 7, "value_hex": "ff000000"}]}
    A variable already present is replaced in place; a new one is appended at
    the end of the record run.
    """
    script = json.loads(pathlib.Path(script_path).read_text(encoding="utf-8"))
    starts = find_store_start(buf)
    if not starts:
        print("could not locate a variable store", file=sys.stderr)
        return 2
    start = starts[-1]
    recs = walk(buf, start)
    if not recs:
        print("store head has no records", file=sys.stderr)
        return 2
    end = recs[-1]["offset"] + recs[-1]["size"]

    data = bytearray(buf)
    by_name = {r["name"]: r for r in recs}

    for spec in script.get("sets", []):
        name = spec["name"]
        guid = spec.get("guid", APPLE_BOOT_GUID)
        attributes = int(spec.get("attributes", 7))
        value = bytes.fromhex(spec["value_hex"])
        new_rec = encode_record(guid, attributes, name, value)
        existing = by_name.get(name)
        if existing and existing["size"] == len(new_rec):
            data[existing["offset"] : existing["offset"] + existing["size"]] = new_rec
            print(
                f"replaced {name!r} in place at {existing['offset']} "
                f"({len(new_rec)} B, same size)"
            )
        elif existing:
            print(
                f"WARNING: {name!r} exists at {existing['offset']} with size "
                f"{existing['size']} but the new record is {len(new_rec)}; "
                "in-place replace would shift every later offset",
                file=sys.stderr,
            )
            return 3
        else:
            data[end:end] = new_rec
            print(f"appended {name!r} at {end} ({len(new_rec)} B); store end -> {end + len(new_rec)}")
            end += len(new_rec)

    pathlib.Path(out_path).write_bytes(bytes(data))
    print(f"wrote {out_path} ({len(data)} bytes, was {len(buf)})")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help="path to a .nvram file")
    ap.add_argument("--list", action="store_true", help="list every variable in the store")
    ap.add_argument("--find", metavar="NAME", help="show a single variable's raw bytes")
    ap.add_argument("--hex", action="store_true", help="with --find, skip the decoded line")
    ap.add_argument("--rebuild", nargs=2, metavar=("OUT", "SCRIPT"), help="apply a JSON edit script")
    args = ap.parse_args(argv)

    buf = pathlib.Path(args.file).read_bytes()
    if args.rebuild:
        return cmd_rebuild(buf, args.rebuild[0], args.rebuild[1])
    if args.find:
        return cmd_find(buf, args.find, args.hex)
    return cmd_list(buf)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
