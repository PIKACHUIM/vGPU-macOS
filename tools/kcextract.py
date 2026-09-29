#!/usr/bin/env python3
"""Pull one kext's Mach-O image back out of a kernel collection.

Why this exists: on macOS 11 and later the kext binaries no longer live in the filesystem.
``/System/Library/Extensions/IOGraphicsFamily.kext`` is a 12 KB shell holding only
``Info.plist``, ``version.plist`` and ``_CodeSignature`` -- ``Contents/MacOS`` is absent. The
code sits inside ``/System/Library/KernelCollections/SystemKernelExtensions.kc``.

That is a problem for anything that has to link against such a family. A kernel collection
is a Mach-O of filetype ``MH_FILESET`` (0xc) whose load commands include one
``LC_FILESET_ENTRY`` (0x80000035) per kext, each naming a bundle identifier and the file
offset where a complete nested Mach-O begins. Extracting one is therefore just: read the
fileset, find the entry, copy the nested image out -- and then fix up the file offsets,
because inside a collection every segment's ``fileoff`` is absolute within the collection
rather than relative to the image.

Usage:
    python3 kcextract.py <kc> --list [--grep SUBSTR]
    python3 kcextract.py <kc> --id com.apple.iokit.IOGraphicsFamily --out IOGraphicsFamily.macho
"""

from __future__ import annotations

import argparse
import mmap
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
MH_FILESET = 0xC

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_DYSYMTAB = 0x0B
LC_CODE_SIGNATURE = 0x1D
LC_SEGMENT_SPLIT_INFO = 0x1E
LC_FUNCTION_STARTS = 0x26
LC_DATA_IN_CODE = 0x29
LC_DYLIB_CODE_SIGN_DRS = 0x2B
LC_LINKER_OPTIMIZATION_HINT = 0x2E
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_FILESET_ENTRY = 0x80000035

# Commands whose payload is one (dataoff, datasize) pair at +8/+12.
LINKEDIT_DATA = {
    LC_CODE_SIGNATURE, LC_SEGMENT_SPLIT_INFO, LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
    LC_DYLIB_CODE_SIGN_DRS, LC_LINKER_OPTIMIZATION_HINT, LC_DYLD_EXPORTS_TRIE,
    LC_DYLD_CHAINED_FIXUPS,
}

# dysymtab_command: the fields that are file offsets, at these offsets within the command.
DYSYMTAB_OFFSET_FIELDS = (48, 56, 64, 72, 80, 88)


def read_header(buf, off):
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from(
        "<IiIIIIII", buf, off)
    return {"off": off, "magic": magic, "filetype": filetype,
            "ncmds": ncmds, "sizeofcmds": sizeofcmds}


def iter_commands(buf, hdr):
    off = hdr["off"] + 32
    for _ in range(hdr["ncmds"]):
        cmd, cmdsize = struct.unpack_from("<II", buf, off)
        if cmdsize < 8:
            raise SystemExit(f"malformed load command at {off}: cmdsize {cmdsize}")
        yield off, cmd, cmdsize
        off += cmdsize


def lc_str(buf, cmd_off, field_off):
    """Read a `union lc_str`: u32 offset relative to the start of the load command itself,
    then a NUL-terminated string. Measured against LC_FILESET_ENTRY: the field at +24 holds
    0x20 for a command whose text begins at +32, not at +24+0x20."""
    delta = struct.unpack_from("<I", buf, cmd_off + field_off)[0]
    start = cmd_off + delta
    end = buf.find(b"\x00", start)
    if end < 0:
        raise SystemExit(f"unterminated string at {start}")
    return buf[start:end].decode("utf-8", "replace")


def fileset_entries(buf):
    hdr = read_header(buf, 0)
    if hdr["magic"] != MH_MAGIC_64:
        raise SystemExit(f"not a 64-bit Mach-O (magic 0x{hdr['magic']:08x})")
    if hdr["filetype"] != MH_FILESET:
        raise SystemExit(f"not a fileset (filetype 0x{hdr['filetype']:x})")
    entries = []
    for off, cmd, cmdsize in iter_commands(buf, hdr):
        if cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff = struct.unpack_from("<QQ", buf, off + 8)
            entries.append({"id": lc_str(buf, off, 24), "fileoff": fileoff, "vmaddr": vmaddr})
    entries.sort(key=lambda e: e["fileoff"])
    return entries


def image_extent(buf, base, ceiling):
    """Absolute end offset of the nested Mach-O starting at `base`, plus its segment table."""
    hdr = read_header(buf, base)
    end = base + 32 + hdr["sizeofcmds"]
    segments = []
    for off, cmd, cmdsize in iter_commands(buf, hdr):
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8:off + 24].rstrip(b"\x00").decode("utf-8", "replace")
            seg_fileoff = struct.unpack_from("<Q", buf, off + 40)[0]
            seg_filesize = struct.unpack_from("<Q", buf, off + 48)[0]
            segments.append((name, seg_fileoff, seg_filesize))
            if seg_filesize:
                end = max(end, seg_fileoff + seg_filesize)
        elif cmd in LINKEDIT_DATA:
            dataoff = struct.unpack_from("<I", buf, off + 8)[0]
            datasize = struct.unpack_from("<I", buf, off + 12)[0]
            if datasize:
                # The chained fixups, export trie and code signature live past the end of
                # __LINKEDIT proper, so segments alone do not bound the image.
                end = max(end, dataoff + datasize)
    # Clamp to the collection, NOT to the next fileset entry. Measured on SystemKC: a kext's
    # __TEXT/__DATA/__DATA_CONST sit with their neighbours, but __LINKEDIT lives at the far
    # end of the collection (IOGraphicsFamily's is at 353042432 while the next entry starts
    # at 346148864). Trimming to the next entry therefore discards the symbol table, the
    # export trie and the chained fixups, and the result reads as a malformed Mach-O.
    return min(end, len(buf)), segments


def rebase(img: bytearray, base: int) -> int:
    """Rewrite file offsets from collection-absolute to image-relative. Returns count fixed."""
    hdr = read_header(img, 0)
    fixed = 0
    for off, cmd, cmdsize in iter_commands(img, hdr):
        if cmd == LC_SEGMENT_64:
            field = off + 40
            value = struct.unpack_from("<Q", img, field)[0]
            if value >= base:
                struct.pack_into("<Q", img, field, value - base)
                fixed += 1
            # A segment command is followed by nsects section_64 records, each carrying its
            # own file offset and relocation-table offset. Missing these is what makes the
            # result read as "truncated or malformed object": the section offsets still point
            # into the middle of the collection.
            nsects = struct.unpack_from("<I", img, off + 64)[0]
            section = off + 72
            for _ in range(nsects):
                for delta in (48, 56):  # section_64.offset, section_64.reloff
                    here = section + delta
                    inner = struct.unpack_from("<I", img, here)[0]
                    if inner and inner >= base:
                        struct.pack_into("<I", img, here, inner - base)
                        fixed += 1
                section += 80
        elif cmd in LINKEDIT_DATA:
            field = off + 8
            value = struct.unpack_from("<I", img, field)[0]
            if value >= base:
                struct.pack_into("<I", img, field, value - base)
                fixed += 1
        elif cmd == LC_SYMTAB:
            for field in (off + 8, off + 16):
                value = struct.unpack_from("<I", img, field)[0]
                if value >= base:
                    struct.pack_into("<I", img, field, value - base)
                    fixed += 1
        elif cmd == LC_DYSYMTAB:
            for delta in DYSYMTAB_OFFSET_FIELDS:
                field = off + delta
                value = struct.unpack_from("<I", img, field)[0]
                if value >= base:
                    struct.pack_into("<I", img, field, value - base)
                    fixed += 1
    return fixed


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kc", help="path to a .kc kernel collection")
    ap.add_argument("--list", action="store_true", help="list every kext in the collection")
    ap.add_argument("--grep", help="with --list, only show identifiers containing this")
    ap.add_argument("--id", help="bundle identifier of the kext to extract")
    ap.add_argument("--at", type=int, metavar="FILEOFF",
                    help="extract whatever image starts at this collection-absolute offset")
    ap.add_argument("--out", help="path to write the Mach-O image to")
    ap.add_argument("--raw", type=int, metavar="N",
                    help="dump the first N LC_FILESET_ENTRY commands as hex and stop")
    args = ap.parse_args(argv)

    with open(args.kc, "rb") as handle:
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as buf:
            entries = fileset_entries(buf)

            if args.raw:
                hdr = read_header(buf, 0)
                seen = 0
                for off, cmd, cmdsize in iter_commands(buf, hdr):
                    if cmd != LC_FILESET_ENTRY:
                        continue
                    print(f"LC_FILESET_ENTRY at {off} cmdsize {cmdsize}")
                    chunk = bytes(buf[off:off + cmdsize])
                    for k in range(0, len(chunk), 16):
                        row = chunk[k:k + 16]
                        print("  +%02d  %-47s |%s|" % (
                            k,
                            " ".join(f"{x:02x}" for x in row),
                            "".join(chr(x) if 32 <= x < 127 else "." for x in row)))
                    seen += 1
                    if seen >= args.raw:
                        break
                return 0

            if args.at is not None:
                base = args.at
                index = next((i for i, e in enumerate(entries) if e["fileoff"] == base), None)
                ceiling = (entries[index + 1]["fileoff"]
                           if index is not None and index + 1 < len(entries) else len(buf))
                end, segments = image_extent(buf, base, ceiling)
                img = bytearray(buf[base:end])
                fixed = rebase(img, base)
                out = args.out or f"image-0x{base:x}.macho"
                with open(out, "wb") as fh:
                    fh.write(img)
                header = read_header(img, 0)
                print(f"image at {base} (0x{base:x}), ceiling {ceiling}")
                print(f"  {len(img)} bytes -> {out}")
                print(f"  mach-o filetype 0x{header['filetype']:x} ncmds {header['ncmds']}")
                for name, seg_off, seg_size in segments:
                    print(f"    {name:<16} fileoff {seg_off:>12} filesize {seg_size:>10}")
                print(f"  offsets {fixed} rebased")
                return 0

            if args.list or not args.id:
                shown = [e for e in entries if not args.grep or args.grep in e["id"]]
                print(f"{args.kc}: {len(entries)} kexts"
                      + (f", {len(shown)} matching {args.grep!r}" if args.grep else ""))
                for e in shown:
                    print(f"  {e['fileoff']:>12} 0x{e['fileoff']:08x}  {e['id']}")
                return 0

            match = next((e for e in entries if e["id"] == args.id), None)
            if not match:
                sys.exit(f"{args.id} is not in {args.kc}")
            index = entries.index(match)
            ceiling = entries[index + 1]["fileoff"] if index + 1 < len(entries) else len(buf)

            base = match["fileoff"]
            end, segments = image_extent(buf, base, ceiling)
            img = bytearray(buf[base:end])
            fixed = rebase(img, base)

    out = args.out or (args.id.rsplit(".", 1)[-1] + ".macho")
    with open(out, "wb") as fh:
        fh.write(img)

    header = read_header(img, 0)
    print(f"{args.id}")
    print(f"  source      {args.kc} @ {base} (0x{base:x}), ceiling {ceiling}")
    print(f"  image       {len(img)} bytes -> {out}")
    print(f"  mach-o      filetype 0x{header['filetype']:x} ncmds {header['ncmds']}")
    for name, seg_off, seg_size in segments:
        print(f"    {name:<16} fileoff {seg_off:>12} filesize {seg_size:>10}")
    print(f"  offsets     {fixed} file offset(s) rebased")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
