#!/usr/bin/env python3
"""Render a PNG as ASCII art so a text-only agent can judge what is on the screen.

Why this exists (2026-11-03, section 38.21): the agent driving these experiments cannot
look at an image. Statistics alone are not enough to tell "the desktop is up" from
"a flat grey field with noise on it" -- both report tens of thousands of distinct colours.
A coarse luminance rendering settles it: menu bar across the top, window edges, the Dock
strip at the bottom, and readable-ish text patterns are all obvious at 96x30.

Usage:
    python tools/pngascii.py .shots/stage0b/f03.png
    python tools/pngascii.py shot.png --width 120 --crop 0,0,1024,200
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

CHARS = " .:-=+*#%@"


def read_png(path: pathlib.Path) -> tuple[int, int, list[bytearray]]:
    """Decode an 8-bit RGB / RGBA non-interlaced PNG into rows of RGB bytes."""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"{path} is not a PNG")

    pos = 8
    idat = bytearray()
    width = height = depth = colour = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        tag = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour not in (2, 6):
                raise ValueError(f"unsupported PNG: depth {depth}, colour type {colour}")
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + length

    bpp = 3 if colour == 2 else 4
    stride = width * bpp
    raw = zlib.decompress(bytes(idat))

    rows: list[bytearray] = []
    prev = bytearray(stride)
    off = 0
    for _ in range(height):
        filt = raw[off]
        off += 1
        line = bytearray(raw[off : off + stride])
        off += stride
        if filt == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        rows.append(line)
        prev = line

    if bpp == 4:
        rows = [bytearray(b for i, b in enumerate(row) if i % 4 != 3) for row in rows]
    return width, height, rows


def render(rows: list[bytearray], width: int, height: int, cols: int, rows_out: int,
           crop: tuple[int, int, int, int] | None) -> str:
    x0, y0, x1, y1 = crop or (0, 0, width, height)
    x1, y1 = min(x1, width), min(y1, height)
    cw, ch = x1 - x0, y1 - y0
    out = []
    for cy in range(rows_out):
        y = y0 + cy * ch // rows_out
        line = rows[y]
        buf = []
        for cx in range(cols):
            x = x0 + cx * cw // cols
            o = x * 3
            lum = (line[o] * 299 + line[o + 1] * 587 + line[o + 2] * 114) // 1000
            buf.append(CHARS[min(len(CHARS) - 1, lum * len(CHARS) // 256)])
        out.append("".join(buf))
    return "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("png")
    ap.add_argument("--width", type=int, default=96, help="character columns")
    ap.add_argument("--height", type=int, default=30, help="character rows")
    ap.add_argument("--crop", default=None, metavar="X0,Y0,X1,Y1")
    args = ap.parse_args(argv)

    crop = tuple(int(v) for v in args.crop.split(",")) if args.crop else None
    width, height, rows = read_png(pathlib.Path(args.png))
    print(f"# {args.png}  {width}x{height}")
    print(render(rows, width, height, args.width, args.height, crop))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
