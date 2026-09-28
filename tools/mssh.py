#!/usr/bin/env python3
"""Non-interactive SSH runner for the vGPU-macOS test guest.

Credentials are read from environment variables, or from a .env.local file
at the repository root (git-ignored). Never hardcode them here.

Required keys: VG_HOST, VG_USER, VG_PASS (optional: VG_PORT, defaults to 22)

Examples:
    python tools/mssh.py -- uname -a
    python tools/mssh.py -- "sw_vers; echo ---; csrutil status"
    python tools/mssh.py --put ./local/file /tmp/remote/file
    echo "whoami" | python tools/mssh.py
"""

import argparse
import os
import pathlib
import shlex
import sys
import time

try:
    import paramiko
except ImportError:  # pragma: no cover
    sys.exit("paramiko is required: pip install paramiko")

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_ENV_FILE = REPO_ROOT / ".env.local"


def load_env_file(path: pathlib.Path) -> None:
    """Populate os.environ from a KEY=VALUE file without overriding real env."""
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip("'\""))


def connect(connect_timeout: float = 30.0, retries: int = 1, retry_delay: float = 10.0):
    """Open an SSH session, optionally retrying while the guest is still booting.

    A VMware macOS guest can have sshd's listening socket open long before it
    answers, which surfaces as "Error reading SSH protocol banner". paramiko's
    banner timeout defaults to 15 s, far too short for a guest under load, so
    every timeout here is derived from connect_timeout.
    """
    host = os.environ.get("VG_HOST")
    user = os.environ.get("VG_USER")
    password = os.environ.get("VG_PASS")
    port = int(os.environ.get("VG_PORT", "22"))
    missing = [k for k, v in (("VG_HOST", host), ("VG_USER", user), ("VG_PASS", password)) if not v]
    if missing:
        sys.exit(f"missing credentials: {', '.join(missing)} (set them or create {DEFAULT_ENV_FILE})")

    last: Exception | None = None
    for attempt in range(1, max(1, retries) + 1):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            client.connect(
                hostname=host,
                port=port,
                username=user,
                password=password,
                look_for_keys=False,
                allow_agent=False,
                timeout=connect_timeout,
                banner_timeout=connect_timeout,
                auth_timeout=connect_timeout,
            )
            return client
        except Exception as exc:  # noqa: BLE001 - retry any transport-level failure
            last = exc
            client.close()
            if attempt < retries:
                print(
                    f"[mssh] connect attempt {attempt}/{retries} failed "
                    f"({type(exc).__name__}: {exc}); retrying in {retry_delay:.0f}s",
                    file=sys.stderr,
                )
                time.sleep(retry_delay)
    raise SystemExit(f"ssh connect failed after {retries} attempt(s): {last}")


def run(client, command: str, timeout: float | None,
        use_sudo: bool = False, password: str | None = None) -> int:
    if use_sudo:
        # -S reads the password from stdin, -p '' suppresses the prompt text.
        # The password stays off the remote command line.
        remote = f"sudo -S -p '' bash -c {shlex.quote(command)}"
    else:
        remote = command

    stdin, stdout, stderr = client.exec_command(remote, timeout=timeout, get_pty=False)
    if use_sudo:
        stdin.write((password or "") + "\n")
        stdin.flush()
        stdin.channel.shutdown_write()

    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    status = stdout.channel.recv_exit_status()
    if out:
        sys.stdout.write(out)
    if err:
        sys.stderr.write(err)
    return status


def put(client, local: str, remote: str) -> int:
    sftp = client.open_sftp()
    try:
        sftp.put(local, remote)
    finally:
        sftp.close()
    print(f"uploaded {local} -> {remote}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True, description=__doc__)
    ap.add_argument("-e", "--env-file", default=str(DEFAULT_ENV_FILE))
    ap.add_argument("-t", "--timeout", type=float, default=120.0)
    ap.add_argument("-c", "--connect-timeout", type=float, default=30.0,
                    help="TCP/banner/auth timeout for the SSH handshake (default 30s)")
    ap.add_argument("-r", "--retries", type=int, default=1,
                    help="connect attempts, useful for polling a booting guest")
    ap.add_argument("--put", nargs=2, metavar=("LOCAL", "REMOTE"))
    ap.add_argument("-s", "--sudo", action="store_true",
                    help="run the remote command under sudo -S (password fed on stdin)")
    ap.add_argument("command", nargs="*", help="remote command (joined by spaces)")
    args = ap.parse_args()

    load_env_file(pathlib.Path(args.env_file))

    command = " ".join(args.command).strip()
    if not command and not args.put:
        command = sys.stdin.read().strip()
    if not command and not args.put:
        sys.exit("no command given")

    client = connect(args.connect_timeout, args.retries)
    try:
        if args.put:
            return put(client, args.put[0], args.put[1])
        return run(client, command, args.timeout,
                   use_sudo=args.sudo, password=os.environ.get("VG_PASS"))
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
