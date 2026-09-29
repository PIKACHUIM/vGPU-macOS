#!/usr/bin/env python3
"""List or extract files from an ISO9660 image (no dependencies).

    python3 isols.py <image.iso>                 # list
    python3 isols.py <image.iso> <member>...     # extract, creating directories

Written because the host has no 7-Zip and no isoinfo, and because the interesting
question about VMware's darwin.iso is what kernel extensions VMware ships for a
macOS guest. Only ISO9660 (with Rock Ridge ignored, names taken as-is) is handled,
which is what VMware's tools images are.
"""

import os
import struct
import sys

SECTOR = 2048
SYSTEM_AREA = 16          # sector number of the primary volume descriptor
DIR_FLAGS = 0x02          # directory record flag

ROOT_IDENT = b"\x00"


def read_dir_records(data):
    records = []
    offset = 0
    while offset < len(data):
        length = data[offset]
        if length == 0:
            # Records are word aligned within the sector; a zero length means skip
            # to the next sector boundary.
            offset = (offset // SECTOR + 1) * SECTOR
            continue
        record = data[offset:offset + length]
        offset += length

        extent = struct.unpack_from("<I", record, 2)[0]
        size = struct.unpack_from("<I", record, 10)[0]
        flags = record[25]
        name_len = record[32]
        name = record[33:33 + name_len]
        if name[:1] == ROOT_IDENT:
            name = b"."
        elif name == b"\x01":
            name = b".."
        # Strip the ISO9660 version suffix ";1".
        if b";" in name:
            name = name.split(b";")[0]
        records.append((name.decode("ascii", "replace"), extent, size, bool(flags & DIR_FLAGS)))
    return records


def load_extent(image, extent, size):
    with open(image, "rb") as handle:
        handle.seek(extent * SECTOR)
        return handle.read(size)


def walk(image, dir_extent, dir_size, prefix, out):
    data = load_extent(image, dir_extent, dir_size)
    for name, extent, size, is_dir in read_dir_records(data):
        if name in (".", ".."):
            continue
        path = prefix + name
        if is_dir:
            out.append((path + "/", extent, size, True))
            walk(image, extent, size, path + "/", out)
        else:
            out.append((path, extent, size, False))


def main(argv):
    # argv[0] is the image; any further entries are members to extract.
    if not argv:
        print(__doc__)
        return 2
    image = argv[0]
    extract_names = argv[1:]

    pvd = load_extent(image, SYSTEM_AREA, SECTOR)
    if pvd[1:6] != b"CD001":
        print(f"{image}: not an ISO9660 image (no CD001 signature)")
        return 2
    root = pvd[156:156 + 34]
    extent, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]

    entries = []
    walk(image, extent, size, "/", entries)

    if not extract_names:
        for path, _extent, size, is_dir in sorted(entries):
            print(f"{'d' if is_dir else '-'} {size:>10}  {path}")
        print(f"\n{len(entries)} entries")
        return 0

    wanted = set(extract_names)
    matched = 0
    for path, extent, size, is_dir in entries:
        clean = path.rstrip("/")
        if not any(clean == w or clean.endswith("/" + w) for w in wanted):
            continue
        matched += 1
        target = clean.lstrip("/")
        data = load_extent(image, extent, size)
        if is_dir or path.endswith("/"):
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        with open(target, "wb") as handle:
            handle.write(data)
        print(f"extracted {clean} ({size} bytes)")
    if not matched:
        print("nothing matched:", ", ".join(extract_names))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
