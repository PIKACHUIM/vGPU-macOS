#!/usr/bin/env python3
"""Grab a screenshot of a VMware guest's console over its built-in VNC server.

Why this exists: the guest runs VMware Workstation's macOS support, which ships no VMware
Tools. That means every Vix guest operation is unavailable --
`vmrun captureScreen`, `runProgramInGuest`, all of it returns
``VIX_E_TOOLS_NOT_RUNNING``. With no console there is no way to see what a bootloader is
printing, or to tell a black screen from a panicked one.

VMware's own VNC server (``RemoteDisplay.vnc.enabled`` in the .vmx) needs nothing from the
guest. The RFB handshake is standard, and when a password is configured it uses classic VNC
authentication: a DES-encrypted 16-byte challenge, with the password used as a fixed key that
has each byte bit-reversed. pyDes supplies the DES.

Configuration comes from the environment or .env.local:

    VG_VNC_HOST   (default 127.0.0.1)
    VG_VNC_PORT   (default 5901)
    VG_VNC_PASS   (empty means the server offers security type 1, "None")

Usage:
    python tools/vncsnap.py out.png
    python tools/vncsnap.py out.png --host 192.168.9.131 --port 5901
"""

from __future__ import annotations

import argparse
import os
import pathlib
import socket
import struct
import sys
import zlib

try:
    from pyDes import ECB, des
except ImportError:  # pragma: no cover
    sys.exit("pyDes is required: pip install pyDes")

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def load_env_file(path: pathlib.Path) -> None:
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip("'\""))


def recv_exact(sock: socket.socket, count: int) -> bytes:
    buf = bytearray()
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            raise ConnectionError(f"connection closed after {len(buf)}/{count} bytes")
        buf += chunk
    return bytes(buf)


def vnc_key(password: str) -> bytes:
    """Classic VNC key: the password, NUL-padded to 8 bytes, each byte bit-reversed."""
    raw = password.encode("latin-1", "replace")[:8].ljust(8, b"\x00")
    return bytes(int(f"{byte:08b}"[::-1], 2) for byte in raw)


def authenticate(sock: socket.socket, password: str) -> str:
    banner = recv_exact(sock, 12)
    if not banner.startswith(b"RFB 003."):
        raise ConnectionError(f"not an RFB server: {banner!r}")
    sock.sendall(b"RFB 003.008\n")

    count = recv_exact(sock, 1)[0]
    if count == 0:
        reason_len = struct.unpack(">I", recv_exact(sock, 4))[0]
        raise ConnectionError(f"server refused: {recv_exact(sock, reason_len).decode('utf-8', 'replace')}")
    offered = list(recv_exact(sock, count))

    if 2 in offered:
        if not password:
            raise ConnectionError("server wants VNC auth but VG_VNC_PASS is empty")
        sock.sendall(b"\x02")
        challenge = recv_exact(sock, 16)
        sock.sendall(des(vnc_key(password), ECB).encrypt(challenge))
        result = struct.unpack(">I", recv_exact(sock, 4))[0]
        if result != 0:
            raise ConnectionError(f"VNC authentication failed (result {result})")
        return "VNC auth"
    if 1 in offered:
        sock.sendall(b"\x01")
        return "none"
    raise ConnectionError(f"no supported security type in {offered}")


def grab(host: str, port: int, password: str) -> tuple[int, int, list[bytes], str]:
    with socket.create_connection((host, port), timeout=15) as sock:
        sock.settimeout(30)
        auth = authenticate(sock, password)

        sock.sendall(b"\x01")  # ClientInit, shared
        width, height = struct.unpack(">HH", recv_exact(sock, 4))
        recv_exact(sock, 16)  # server pixel format, we override it below
        name_len = struct.unpack(">I", recv_exact(sock, 4))[0]
        name = recv_exact(sock, name_len).decode("utf-8", "replace")

        # SetPixelFormat: message type 0, 3 pad, then the 16-byte PIXEL_FORMAT --
        # 32 bpp, depth 24, little-endian, true colour, RGB in bits 16/8/0, 3 pad.
        sock.sendall(
            struct.pack(">BBBBBBBBHHHBBB",
                        0,                          # message type
                        0, 0, 0,                    # padding
                        32, 24, 0, 1,               # bpp, depth, big-endian, true-colour
                        255, 255, 255,              # red/green/blue max
                        16, 8, 0)                   # red/green/blue shift
            + b"\x00\x00\x00"
        )
        sock.sendall(struct.pack(">BBH", 2, 0, 1) + struct.pack(">i", 0))  # SetEncodings: Raw
        sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))   # full update

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
        return width, height, [bytes(r) for r in rows], f"{auth}, {name or 'unnamed'}"


def write_png(path: pathlib.Path, width: int, height: int, rows: list[bytes]) -> None:
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="path to write the PNG to")
    ap.add_argument("--env-file", default=str(REPO_ROOT / ".env.local"))
    ap.add_argument("--host", default=None, help="VNC host (default VG_VNC_HOST or 127.0.0.1)")
    ap.add_argument("--port", type=int, default=None, help="VNC port (default VG_VNC_PORT or 5901)")
    args = ap.parse_args(argv)

    load_env_file(pathlib.Path(args.env_file))
    host = args.host or os.environ.get("VG_VNC_HOST", "127.0.0.1")
    port = args.port or int(os.environ.get("VG_VNC_PORT", "5901"))
    password = os.environ.get("VG_VNC_PASS", "")

    width, height, rows, info = grab(host, port, password)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_png(out, width, height, rows)
    print(f"{host}:{port} -> {out}  {width}x{height}  ({info})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
