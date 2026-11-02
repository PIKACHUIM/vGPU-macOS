#!/usr/bin/env python3
"""Keep one VNC connection open and grab a series of frames from it.

Why this exists (2026-11-03, section 38.17): ``vncsnap.py`` opens a connection, grabs one
frame, and closes. VMware's MKS treats that close as the last console going away --
``MKSRemoteMgr: CleanupConnection (numConsoles=0)`` -- and the ScreenMgr reacts by tearing
down the display the guest declared (``Guest display topology changed: numDisplays 0``,
``SWBScreen: Screen 1 Destroyed``) and falling back to a 1024x512 ScreenTarget.

That means a per-frame reconnect loop *destroys the very thing it is trying to observe*.
Every sample after the first tears the display down and the next sample sees the
fallback. Any experiment that samples in a loop has to hold the connection open.

Usage:
    python tools/vncloop.py OUTDIR --count 40 --interval 1.0
    python tools/vncloop.py OUTDIR --count 40 --interval 1.0 --prefix t
"""

from __future__ import annotations

import argparse
import pathlib
import socket
import struct
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from vncsnap import (  # noqa: E402
    authenticate,
    load_env_file,
    recv_exact,
    write_png,
)

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class FrameBuffer:
    """A persistent RGB framebuffer that FramebufferUpdate rectangles get patched into.

    Section 38.21 got burned by the naive version of this: the server answers an
    ``incremental=0`` full request with *only the rectangles it considers changed*
    (measured: a 20x20 corner rect, ``nonzero=512`` out of 786432 pixels) even though the
    screen had a full desktop on it. Patching those deltas into a buffer that starts as
    zeros therefore reported "almost black" for a screen that was fine.

    Two consequences, both fixed here:

    * rectangles are patched at ``(x, y)`` into the real screen, not written to row 0;
    * the caller can tell that a frame is a *delta* rather than a full repaint, so a
      sparse frame is reported as "unchanged since <frame>" instead of "black".

    ``fresh`` is True only when the frame covered the whole screen from a blank buffer.
    """

    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.rows = [bytearray(width * 3) for _ in range(height)]
        self.last_full_frame = -1
        self.patched = 0        # pixels written by the most recent update
        self.rects = 0          # rectangles in the most recent update
        self.fresh = False      # was that update a complete repaint?

    def apply(self, sock: socket.socket, index: int) -> None:
        """Read one FramebufferUpdate off ``sock`` and patch it in."""
        self.patched = 0
        self.rects = 0
        spans: list[tuple[int, int, int]] = []   # (y, x0, x1) of every row touched

        for _ in range(_framebuffer_update_rects(sock)):
            x, y, w, h, encoding = struct.unpack(">HHHHi", recv_exact(sock, 12))
            if encoding != 0:
                raise ConnectionError(f"server used encoding {encoding}, only Raw is supported")
            pixels = recv_exact(sock, w * h * 4)
            for row in range(h):
                self._blit(x, y + row, w, pixels[row * w * 4 : (row + 1) * w * 4])
                spans.append((y + row, x, x + w))
            self.patched += w * h
            self.rects += 1

        self.fresh = _covers_screen(spans, self.width, self.height)
        if self.fresh:
            self.last_full_frame = index

    def _blit(self, x: int, y: int, w: int, pixels: bytes) -> None:
        if not (0 <= y < self.height) or not w:
            return
        x0 = max(0, x)
        x1 = min(self.width, x + w)
        if x1 <= x0:
            return
        dst = self.rows[y]
        for col in range(x0 - x, x1 - x):
            value = int.from_bytes(pixels[col * 4 : col * 4 + 4], "little")
            off = (x + col) * 3
            dst[off + 0] = (value >> 16) & 0xFF
            dst[off + 1] = (value >> 8) & 0xFF
            dst[off + 2] = value & 0xFF

    def stats(self) -> tuple[int, int, int]:
        """(distinct packed colours, non-black pixels, total pixels)."""
        seen = set()
        nonzero = 0
        for row in self.rows:
            for i in range(0, len(row), 3):
                value = (row[i] << 16) | (row[i + 1] << 8) | row[i + 2]
                seen.add(value)
                if value:
                    nonzero += 1
        return len(seen), nonzero, self.width * self.height


def _covers_screen(spans: list[tuple[int, int, int]], width: int, height: int) -> bool:
    """True when the union of the touched row spans tiles the whole screen."""
    if not spans:
        return False
    per_row: dict[int, list[tuple[int, int]]] = {}
    for y, x0, x1 in spans:
        if 0 <= y < height:
            per_row.setdefault(y, []).append((max(0, x0), min(width, x1)))
    if len(per_row) != height:
        return False
    for pieces in per_row.values():
        reached = 0
        for start, end in sorted(pieces):
            if start > reached:
                return False
            reached = max(reached, end)
        if reached < width:
            return False
    return True


def _framebuffer_update_rects(sock: socket.socket) -> int:
    """Skip non-framebuffer messages until a FramebufferUpdate arrives; return its rect count."""
    while True:
        msg_type = recv_exact(sock, 1)[0]
        if msg_type == 0:  # FramebufferUpdate
            break
        if msg_type == 1:  # SetColourMapEntries
            recv_exact(sock, 3)
            count = struct.unpack(">H", recv_exact(sock, 2))[0]
            recv_exact(sock, count * 6)
        elif msg_type == 2:  # Bell
            continue
        elif msg_type == 3:  # ServerCutText
            recv_exact(sock, 3)
            length = struct.unpack(">I", recv_exact(sock, 4))[0]
            recv_exact(sock, length)
        else:
            raise ConnectionError(f"unknown server message type {msg_type}")

    recv_exact(sock, 1)  # padding
    return struct.unpack(">H", recv_exact(sock, 2))[0]


def read_framebuffer(sock: socket.socket, width: int, height: int) -> list[bytes]:
    """Legacy one-shot reader: ask for a full update and return what came back.

    Kept for callers that want a single sparse answer. New code should use FrameBuffer.
    """
    sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))

    while True:
        msg_type = recv_exact(sock, 1)[0]
        if msg_type == 0:  # FramebufferUpdate
            break
        if msg_type == 1:  # SetColourMapEntries
            recv_exact(sock, 3)
            count = struct.unpack(">H", recv_exact(sock, 2))[0]
            recv_exact(sock, count * 6)
        elif msg_type == 2:  # Bell
            continue
        elif msg_type == 3:  # ServerCutText
            recv_exact(sock, 3)
            length = struct.unpack(">I", recv_exact(sock, 4))[0]
            recv_exact(sock, length)
        else:
            raise ConnectionError(f"unknown server message type {msg_type}")

    recv_exact(sock, 1)  # padding
    rect_count = struct.unpack(">H", recv_exact(sock, 2))[0]

    rows = [bytearray(width * 3) for _ in range(height)]
    for _ in range(rect_count):
        x, y, w, h, encoding = struct.unpack(">HHHHi", recv_exact(sock, 12))
        if encoding != 0:
            raise ConnectionError(f"server used encoding {encoding}, only Raw is supported")
        pixels = recv_exact(sock, w * h * 4)
        for row in range(h):
            src = row * w * 4
            dst = rows[y + row]
            for col in range(w):
                value = int.from_bytes(pixels[src + col * 4 : src + col * 4 + 4], "little")
                dst[(x + col) * 3 + 0] = (value >> 16) & 0xFF
                dst[(x + col) * 3 + 1] = (value >> 8) & 0xFF
                dst[(x + col) * 3 + 2] = value & 0xFF
    return [bytes(r) for r in rows]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", help="directory to write frames into")
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--prefix", default="f")
    ap.add_argument("--env-file", default=str(REPO_ROOT / ".env.local"))
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--connect-timeout", type=float, default=45.0)
    args = ap.parse_args(argv)

    load_env_file(pathlib.Path(args.env_file))
    host = args.host or __import__("os").environ.get("VG_VNC_HOST", "127.0.0.1")
    port = args.port or int(__import__("os").environ.get("VG_VNC_PORT", "5901"))
    password = __import__("os").environ.get("VG_VNC_PASS", "")

    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    # The guest takes ~4s to bring VNC up after power-on; wait for it rather than
    # giving up, so the frame numbering starts where the caller expects.
    deadline = time.time() + args.connect_timeout
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=5)
            break
        except OSError:
            time.sleep(0.5)
    if sock is None:
        print("could not connect", file=sys.stderr)
        return 1

    t0 = time.time()
    with sock:
        sock.settimeout(30)
        auth = authenticate(sock, password)
        sock.sendall(b"\x01")  # ClientInit, shared
        width, height = struct.unpack(">HH", recv_exact(sock, 4))
        recv_exact(sock, 16)
        name_len = struct.unpack(">I", recv_exact(sock, 4))[0]
        name = recv_exact(sock, name_len).decode("utf-8", "replace")

        sock.sendall(
            struct.pack(
                ">BBBBBBBBHHHBBB",
                0, 0, 0, 0,
                32, 24, 0, 1,
                255, 255, 255,
                16, 8, 0,
            )
            + b"\x00\x00\x00"
        )
        sock.sendall(struct.pack(">BBH", 2, 0, 1) + struct.pack(">i", 0))  # SetEncodings: Raw
        print(f"connected {width}x{height} ({auth}, {name or 'unnamed'})", flush=True)

        fb = FrameBuffer(width, height)
        for i in range(args.count):
            el = time.time() - t0
            # incremental=0 asks the server for a complete repaint; VMware's MKS may still
            # answer with deltas only (measured, section 38.21), which FrameBuffer patches in.
            sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))
            fb.apply(sock, i)

            path = outdir / f"{args.prefix}{int(el):02d}.png"
            write_png(path, width, height, fb.rows)
            distinct, nonzero, total = fb.stats()
            kind = "FULL" if fb.fresh else f"delta(rects={fb.rects},px={fb.patched})"
            last = "screen" if fb.fresh else f"t+frame {fb.last_full_frame}"
            print(
                f"t+{int(el):3d}s {width}x{height} {kind} distinct={distinct} "
                f"nonzero={nonzero}/{total} since={last} -> {path.name}",
                flush=True,
            )
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
