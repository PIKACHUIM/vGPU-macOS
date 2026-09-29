#!/usr/bin/env python3
"""Send keyboard and mouse input to a VMware guest's console over its built-in VNC server.

Companion to tools/vncsnap.py. Same rationale: a macOS guest under VMware Workstation has
no VMware Tools, so there is no API to log in, to click an approval dialog, or to hand a
command to the GUI session. VMware's own VNC server needs nothing from the guest, so RFB
becomes the only way to drive the console.

RFB input messages (protocol 3.8):
  Client -> server message 4  KeyEvent:     u8 down-flag, 2 pad, u32 keysym
  Client -> server message 5  PointerEvent: u8 button-mask, u16 x, u16 y

A keysym is sent twice -- once with down=1, once with down=0. Modifiers are keysyms too
(Shift 0xffe1, Control 0xffe3, Alt 0xffe9, Command/Meta 0xffeb), so "cmd+space" is four
events: cmd down, space down, space up, cmd up.

Configuration comes from the environment or .env.local, exactly as vncsnap.py:

    VG_VNC_HOST   (default 127.0.0.1)
    VG_VNC_PORT   (default 5901)
    VG_VNC_PASS   (empty means the server offers security type 1, "None")

Usage:
    python tools/vncsend.py --type 'IM0612'
    python tools/vncsend.py --key Return
    python tools/vncsend.py --key cmd+space --type 'Terminal' --key Return
    python tools/vncsend.py --click 512 384
    python tools/vncsend.py --move 512 384
    python tools/vncsend.py --paste-text 'sudo kmutil rebuild'

--paste-text puts the string on the clipboard through the guest's pbcopy and then presses
Command-V. That exists because a long shell command typed one keysym at a time through RFB
is fragile: a single dropped event silently produces a different command, and the mistake
is only visible after it runs. The shell escapes the string for single quotes.

Note on sharing: ClientInit is sent with shared=1 so this can run while another viewer is
attached. VMware's VNC server is happy with that; the guest just sees a second keyboard.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time

try:
    from pyDes import ECB, des
except ImportError:  # pragma: no cover
    sys.exit("pyDes is required: pip install pyDes")

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# --- keysyms ------------------------------------------------------------------------------
# Printable Latin-1 is its own keysym, which covers every ASCII character we need. Named
# keys are the X11 function range (0xff00+).
SHIFT = 0xFFE1
CTRL = 0xFFE3
ALT = 0xFFE9
META = 0xFFEB  # Command on macOS

MODIFIERS = {
    "shift": SHIFT,
    "ctrl": CTRL,
    "control": CTRL,
    "alt": ALT,
    "opt": ALT,
    "option": ALT,
    "cmd": META,
    "command": META,
    "meta": META,
    "super": META,
}

NAMED_KEYS = {
    "return": 0xFF0D,
    "enter": 0xFF0D,
    "tab": 0xFF09,
    "backspace": 0xFF08,
    "escape": 0xFF1B,
    "esc": 0xFF1B,
    "delete": 0xFFFF,
    "space": 0x0020,
    "home": 0xFF50,
    "end": 0xFF57,
    "pageup": 0xFF55,
    "pagedown": 0xFF56,
    "left": 0xFF51,
    "up": 0xFF52,
    "right": 0xFF53,
    "down": 0xFF54,
    "f1": 0xFFBE, "f2": 0xFFBF, "f3": 0xFFC0, "f4": 0xFFC1,
    "f5": 0xFFC2, "f6": 0xFFC3, "f7": 0xFFC4, "f8": 0xFFC5,
    "f9": 0xFFC6, "f10": 0xFFC7, "f11": 0xFFC8, "f12": 0xFFC9,
}

# Characters that need Shift on a US layout, mapped to the unshifted key that produces them.
SHIFTED = {
    "~": "`", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
    "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\",
    ":": ";", '"': "'", "<": ",", ">": ".", "?": "/",
    "A": "a", "B": "b", "C": "c", "D": "d", "E": "e", "F": "f", "G": "g", "H": "h",
    "I": "i", "J": "j", "K": "k", "L": "l", "M": "m", "N": "n", "O": "o", "P": "p",
    "Q": "q", "R": "r", "S": "s", "T": "t", "U": "u", "V": "v", "W": "w", "X": "x",
    "Y": "y", "Z": "z",
}


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


def handshake(host: str, port: int, password: str) -> tuple[socket.socket, int, int, str]:
    sock = socket.create_connection((host, port), timeout=15)
    sock.settimeout(30)
    info = authenticate(sock, password)
    sock.sendall(b"\x01")  # ClientInit, shared
    width, height = struct.unpack(">HH", recv_exact(sock, 4))
    recv_exact(sock, 16)  # server pixel format
    name_len = struct.unpack(">I", recv_exact(sock, 4))[0]
    name = recv_exact(sock, name_len).decode("utf-8", "replace")
    return sock, width, height, f"{info}, {name or 'unnamed'}"


def send_key(sock: socket.socket, keysym: int) -> None:
    sock.sendall(struct.pack(">BBHI", 4, 1, 0, keysym))
    sock.sendall(struct.pack(">BBHI", 4, 0, 0, keysym))


def tap(sock: socket.socket, keysym: int, mods: list[int], delay: float) -> None:
    for mod in mods:
        sock.sendall(struct.pack(">BBHI", 4, 1, 0, mod))
    send_key(sock, keysym)
    for mod in reversed(mods):
        sock.sendall(struct.pack(">BBHI", 4, 0, 0, mod))
    if delay:
        time.sleep(delay)


def char_keysym(ch: str) -> tuple[int, int | None]:
    """Return (keysym, modifier) for one character, or raise if unsupported."""
    if ch in SHIFTED:
        base = SHIFTED[ch]
        return ord(base), SHIFT
    code = ord(ch)
    if code < 0x20 or code > 0x7E:
        raise ValueError(f"character {ch!r} (U+{code:04X}) is not typeable on a US layout")
    return code, None


def type_text(sock: socket.socket, text: str, delay: float) -> None:
    for ch in text:
        keysym, mod = char_keysym(ch)
        tap(sock, keysym, [mod] if mod else [], delay)


def parse_key(spec: str) -> tuple[int, list[int]]:
    """Parse 'cmd+shift+p' into (keysym, [modifier keysyms])."""
    parts = [p.strip().lower() for p in spec.split("+") if p.strip()]
    if not parts:
        raise ValueError("empty key spec")
    *mod_names, final = parts
    mods = []
    for name in mod_names:
        if name not in MODIFIERS:
            raise ValueError(f"unknown modifier {name!r}")
        mods.append(MODIFIERS[name])

    if final in NAMED_KEYS:
        return NAMED_KEYS[final], mods
    if len(final) == 1:
        keysym, extra = char_keysym(final)
        if extra:
            mods.append(extra)
        return keysym, mods
    raise ValueError(f"unknown key {spec!r}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env-file", default=str(REPO_ROOT / ".env.local"))
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--type", dest="text", action="append", default=[],
                    help="type a literal string (repeatable, in order given)")
    ap.add_argument("--key", action="append", default=[],
                    help="press a key, e.g. Return, cmd+space, cmd+shift+p (repeatable)")
    ap.add_argument("--move", nargs=2, type=int, metavar=("X", "Y"), help="move the pointer")
    ap.add_argument("--click", nargs=2, type=int, metavar=("X", "Y"), help="move and click button 1")
    ap.add_argument("--paste-text", metavar="TEXT",
                    help="push TEXT to the guest clipboard, then press Command-V")
    ap.add_argument("--delay", type=float, default=0.03, help="seconds between keys (default 0.03)")
    args = ap.parse_args(argv)

    load_env_file(pathlib.Path(args.env_file))
    host = args.host or os.environ.get("VG_VNC_HOST", "127.0.0.1")
    port = args.port or int(os.environ.get("VG_VNC_PORT", "5901"))
    password = os.environ.get("VG_VNC_PASS", "")

    if args.paste_text and not (os.environ.get("VG_HOST") and os.environ.get("VG_USER")):
        sys.exit("--paste-text needs VG_HOST/VG_USER (it uses ssh + pbcopy inside the guest)")

    # Anything time-ordered must run in one connection: a fresh RFB session per action would
    # release all modifiers between them and defeat key combinations.
    sock, width, height, info = handshake(host, port, password)
    try:
        if args.move or args.click:
            x, y = args.click or args.move
            sock.sendall(struct.pack(">BBHH", 5, 0, x, y))
            time.sleep(0.1)
            if args.click:
                sock.sendall(struct.pack(">BBHH", 5, 1, x, y))
                time.sleep(0.06)
                sock.sendall(struct.pack(">BBHH", 5, 0, x, y))
            print(f"pointer -> {x},{y}{' (click)' if args.click else ''}")

        if args.paste_text:
            # Put the text on the pasteboard through the guest's own pbcopy, so the payload
            # never has to survive a keysym round trip.
            quoted = args.paste_text.replace("'", "'\\''")
            ssh = [
                sys.executable, str(REPO_ROOT / "tools" / "mssh.py"),
                "-c", "30", "--", f"printf '%s' '{quoted}' | pbcopy",
            ]
            proc = subprocess.run(ssh, capture_output=True, text=True)
            if proc.returncode != 0:
                sys.stderr.write(proc.stdout + proc.stderr)
                return proc.returncode
            print(f"clipboard <- {args.paste_text!r} ({len(args.paste_text)} chars)")
            time.sleep(0.4)
            tap(sock, ord("v"), [META], args.delay)

        for item in args.text:
            type_text(sock, item, args.delay)
            print(f"typed {item!r}")

        for spec in args.key:
            keysym, mods = parse_key(spec)
            tap(sock, keysym, mods, args.delay)
            print(f"pressed {spec}")
    finally:
        sock.close()

    print(f"{host}:{port} {width}x{height} ({info})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
