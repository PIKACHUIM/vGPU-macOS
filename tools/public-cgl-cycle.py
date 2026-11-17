#!/usr/bin/env python3
"""以单次 sudo SSH 会话执行公开 CGL 实验，并验证恢复结果。"""

import argparse
import base64
import contextlib
import io
import json
import os
import pathlib
import re
import shlex
import sys
import tempfile
import uuid

BINARY_SHA = "2b6706eba22815dd3c4b359ad30f9c87728d3435ae6295ffd7f1bf64d9322247"
PLIST_SHA = "148f813929040c7ad06f9d7b9f2da0217d36fe0fd307fee1a4cc6e3279a5f737"
TARGET = "/Library/GPUBundles/VGPUDriverDiagnostic.bundle/Contents/MacOS/VGPUDriverDiagnostic"
STAGES = ("context-only", "clear-read", "gpu", "reject-color")
ROOT = pathlib.Path(__file__).resolve().parent.parent

# 此程序由远端系统 Python 执行；所有外部程序均使用参数数组，禁止 shell 插值。
REMOTE_WORKER = r'''
import base64
import hashlib
import json
import os
import pathlib
import re
import signal
import stat
import subprocess
import sys
import tempfile

# macOS 的 /var 是系统符号链接；仅解析我们创建的工作目录。
work = pathlib.Path(sys.argv[1]).resolve(strict=True)
phase = sys.argv[2]
if phase == "run":
    def interrupted(number, frame):
        raise SystemExit(128 + number)
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, interrupted)
cfg = json.loads((work / "request.json").read_text())
target = pathlib.Path(cfg["target"])
bundle = target.parents[2]
plist = bundle / "Contents/Info.plist"
state_path = work / "state.json"
state = json.loads(state_path.read_text()) if state_path.exists() else {
    "backup": str(work / bundle.name), "restore_verified": False,
    "replacement_intent": False, "probes": {}, "hashes": {}
}

def save():
    temp = work / "state.next"
    with temp.open("w") as handle:
        json.dump(state, handle, sort_keys=True)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(str(temp), str(state_path))

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def safe_path(path):
    require(path.is_absolute(), "路径不是绝对路径")
    for item in (path,) + tuple(path.parents):
        require(not item.is_symlink(), "路径含符号链接: " + str(item))
        item.lstat()

def digest(path):
    safe_path(path)
    require(stat.S_ISREG(path.stat().st_mode), "输入不是普通文件: " + str(path))
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

def metadata(path):
    info = path.stat()
    return {"mode": stat.S_IMODE(info.st_mode), "uid": info.st_uid,
            "gid": info.st_gid}

def acl_entries(path):
    # ls -e 显式读取 macOS ACL；读取失败或输出不完整时拒绝继续。
    output = subprocess.check_output(["/bin/ls", "-ldne", str(path)],
                                     env=dict(os.environ, LC_ALL="C")).decode()
    lines = output.splitlines()
    require(bool(lines) and re.match(r"^[d-][rwxStTs-]{9}[ +@]", lines[0]),
            "无法确认 ACL: " + str(path))
    entries = []
    for index, line in enumerate(lines[1:]):
        match = re.fullmatch(r"\s*(\d+): (.+)", line)
        require(match is not None and int(match.group(1)) == index,
                "无法保存 ACL: " + str(path))
        entries.append(match.group(2))
    require(lines[0][10] != "+" or bool(entries), "ACL 输出缺失: " + str(path))
    return entries

def manifest(root):
    result = {}
    for path in [root] + sorted(root.rglob("*")):
        safe_path(path)
        info = path.stat()
        require(stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode),
                "bundle 含特殊文件: " + str(path))
        item = metadata(path)
        item["kind"] = "file" if path.is_file() else "directory"
        item["flags"] = info.st_flags
        item["acl"] = acl_entries(path)
        # 系统 Python 可能未提供 os.listxattr/getxattr。
        names = subprocess.check_output(["/usr/bin/xattr", str(path)]).decode().splitlines()
        item["xattrs"] = {key: bytes.fromhex(subprocess.check_output(
            ["/usr/bin/xattr", "-px", key, str(path)]).decode()).hex()
            for key in sorted(names)}
        if path.is_file():
            item["sha256"] = digest(path)
        result[str(path.relative_to(root))] = item
    return result

def backup_matches(observed, original):
    # ditto 不复制目录 restricted 标记；其余已列出的字段必须相等。
    # 不修改受保护标记；此 manifest 不声称覆盖时间戳等所有 metadata。
    if observed.keys() != original.keys():
        return False
    for name in original:
        actual, expected = dict(observed[name]), dict(original[name])
        if expected["kind"] == "directory":
            actual["flags"] &= ~0x00080000
            expected["flags"] &= ~0x00080000
        if actual != expected:
            return False
    return True

def command(argv):
    subprocess.run(argv, check=True, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

def probe(name, argv):
    out = work / (name + ".stdout")
    err = work / (name + ".stderr")
    previous = state["probes"].get(name)
    require(not previous or previous.get("exit_confirmed") is not False,
            "上次探测退出未确认，保留原证据: " + name)
    record = {"argv": argv, "pid": None, "exit_code": None,
              "exit_confirmed": False, "timeout": False,
              "stdout_path": str(out), "stderr_path": str(err),
              "stdout_base64": "", "stderr_base64": ""}
    state["probes"][name] = record
    with out.open("wb") as stdout, err.open("wb") as stderr:
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=stdout,
                                   stderr=stderr, start_new_session=True)
        record["pid"] = process.pid
        def terminate():
            record["killpg_attempted"] = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except OSError as exc:
                record["killpg_error"] = type(exc).__name__ + ": " + str(exc)
            try:
                record["exit_code"] = process.wait(timeout=5)
                record["exit_confirmed"] = True
            except subprocess.TimeoutExpired:
                record["termination_wait_timeout"] = True
        try:
            save()
            try:
                record["exit_code"] = process.wait(timeout=45)
                record["exit_confirmed"] = True
            except subprocess.TimeoutExpired:
                record["timeout"] = True
                terminate()
            except BaseException:
                terminate()
                raise
        finally:
            # 未退出时这些只是快照；原始输出文件保留，不能宣称完整输出。
            stdout.flush()
            stderr.flush()
            os.fsync(stdout.fileno())
            os.fsync(stderr.fileno())
            record["stdout_base64"] = base64.b64encode(out.read_bytes()).decode()
            record["stderr_base64"] = base64.b64encode(err.read_bytes()).decode()
            record["output_snapshot"] = True
            save()
    require(record["exit_confirmed"] and record["exit_code"] == 0
            and not record["timeout"], "探测失败: " + name)

def verified_inputs():
    for name in ("candidate", "probe", "trust_probe"):
        require(digest(pathlib.Path(cfg[name])) == cfg[name + "_sha256"],
                "输入 SHA 不匹配: " + name)

def check_original():
    require(digest(target) == cfg["binary_sha256"], "原始二进制 SHA 不匹配")
    require(digest(plist) == cfg["plist_sha256"], "原始 plist SHA 不匹配")
    require(metadata(target) == state["original_metadata"], "原始二进制权限不匹配")
    require(metadata(plist) == state["plist_metadata"], "原始 plist 权限不匹配")

def fresh_image(source, expected_sha, attrs):
    safe_path(target.parent)
    fd, name = tempfile.mkstemp(prefix=".public-cgl-", dir=str(target.parent))
    os.close(fd)
    image = pathlib.Path(name)
    state.setdefault("temporary_images", []).append(str(image))
    save()
    command(["/bin/cp", "-p", str(source), str(image)])
    command(["/usr/sbin/chown", str(attrs["uid"]) + ":" + str(attrs["gid"]), str(image)])
    command(["/bin/chmod", format(attrs["mode"], "o"), str(image)])
    require(digest(image) == expected_sha and metadata(image) == attrs,
            "临时映像验证失败")
    with image.open("rb") as handle:
        os.fsync(handle.fileno())
    require(image.stat().st_dev == target.parent.stat().st_dev, "临时映像跨文件系统")
    return image

try:
    if phase == "run":
        require(os.geteuid() == 0, "需要 sudo root")
        require(subprocess.check_output(["/usr/bin/sw_vers", "-buildVersion"]).strip() == b"24G419",
                "系统 build 不是 24G419")
        require(subprocess.check_output(["/usr/bin/uname", "-m"]).strip() == b"x86_64",
                "架构不是 x86_64")
        safe_path(bundle)
        require(work.stat().st_dev == target.parent.stat().st_dev,
                "备份工作目录与目标不在同一文件系统")
        verified_inputs()
        require(digest(target) == cfg["binary_sha256"], "原始二进制 SHA 不匹配")
        require(digest(plist) == cfg["plist_sha256"], "原始 plist SHA 不匹配")
        state["original_metadata"] = metadata(target)
        state["plist_metadata"] = metadata(plist)
        require(state["original_metadata"] == {"mode": 0o755, "uid": 0, "gid": 0},
                "原始二进制不是 root:wheel 755")
        state["original_inode"] = target.stat().st_ino
        state["hashes"]["before"] = digest(target)
        state["hashes"]["plist_before"] = digest(plist)
        state["manifest_scope"] = ["kind", "mode", "uid", "gid", "flags", "acl", "xattrs", "sha256"]
        state["original_manifest"] = manifest(bundle)
        save()
        command(["/usr/bin/ditto", str(bundle), state["backup"]])
        require(backup_matches(manifest(pathlib.Path(state["backup"])), state["original_manifest"]),
                "完整 bundle 备份验证失败")
        state["backup_verified"] = True
        save()
        for name in ("candidate", "probe", "trust_probe"):
            staged = work / name
            command(["/bin/cp", str(cfg[name]), str(staged)])
            os.chown(str(staged), 0, 0)
            os.chmod(str(staged), 0o755)
            require(digest(staged) == cfg[name + "_sha256"], "暂存输入 SHA 不匹配: " + name)
        probe("trust_before", [str(work / "trust_probe"), "VGPUDriverDiagnostic"])
        check_original()
        require(manifest(bundle) == state["original_manifest"], "备份后 bundle 已变化")
        image = fresh_image(work / "candidate", cfg["candidate_sha256"],
                            {"mode": 0o755, "uid": 0, "gid": 0})
        old = target.parent / (".public-cgl-old-" + cfg["token"])
        state["old_inode_path"] = str(old)
        state["old_link_path"] = str(old)
        save()
        os.link(str(target), str(old))
        require(old.stat().st_ino == state["original_inode"], "旧 inode 保留失败")
        state["replacement_intent"] = True
        save()
        command(["/bin/mv", "-f", str(image), str(target)])
        state["hashes"]["replaced"] = digest(target)
        state["hashes"]["plist_replaced"] = digest(plist)
        state["replacement_inode"] = target.stat().st_ino
        save()
        require(state["hashes"]["replaced"] == cfg["candidate_sha256"], "替换后 SHA 不匹配")
        require(state["hashes"]["plist_replaced"] == cfg["plist_sha256"], "替换后 plist 变化")
        require(metadata(target) == {"mode": 0o755, "uid": 0, "gid": 0}, "替换后权限不匹配")
        probe("trust_replaced", [str(work / "trust_probe"), "VGPUDriverDiagnostic"])
        # trust 成功后才允许公开 CGL probe 执行。
        probe("public", [str(work / "probe"), "--" + cfg["stage"]])
        state["experiment_ok"] = True
        save()
    elif phase == "restore":
        state["restore_verified"] = False
        state.pop("restore_error", None)
        save()
        if state["replacement_intent"]:
            backup = pathlib.Path(state["backup"])
            require(state.get("backup_verified"), "没有已验证备份")
            require(backup_matches(manifest(backup), state["original_manifest"]), "恢复备份已变化")
            source = backup / target.relative_to(bundle)
            image = fresh_image(source, cfg["binary_sha256"], state["original_metadata"])
            state["restoration_inode"] = image.stat().st_ino
            require(image.stat().st_ino != state["original_inode"], "恢复未使用全新 inode")
            save()
            command(["/bin/mv", "-f", str(image), str(target)])
            check_original()
            require(target.stat().st_ino == state["restoration_inode"], "恢复 rename 未生效")
            old_evidence = work / "original.inode"
            old = pathlib.Path(state["old_inode_path"])
            # 兼容归档已完成但 state 尚未保存，以及 state 已保存但旧链接尚未删除。
            if not old.exists() and old_evidence.exists():
                old = old_evidence
            require(old.stat().st_ino == state["original_inode"] and digest(old) == cfg["binary_sha256"],
                    "旧 inode 证据丢失")
            if not old_evidence.exists():
                os.link(str(old), str(old_evidence))
            require(old_evidence.stat().st_ino == state["original_inode"] and
                    digest(old_evidence) == cfg["binary_sha256"], "旧 inode 归档验证失败")
            state["old_inode_path"] = str(old_evidence)
            save()
            # 仅处理本轮明确命名的旧链接，不扩大清理范围。
            old_link = target.parent / (".public-cgl-old-" + cfg["token"])
            require(state.get("old_link_path", str(old_link)) == str(old_link),
                    "旧链接记录不匹配")
            if old_link.exists():
                require(old_link.stat().st_ino == state["original_inode"] and
                        digest(old_link) == cfg["binary_sha256"], "旧链接已变化")
                old_link.unlink()
            for name in state.get("temporary_images", []):
                path = pathlib.Path(name)
                if path.exists():
                    safe_path(path)
                    path.unlink()
            require(manifest(bundle) == state["original_manifest"], "恢复后 bundle 验证失败")
            require(digest(work / "trust_probe") == cfg["trust_probe_sha256"], "恢复 trust probe SHA 变化")
            probe("trust_restored", [str(work / "trust_probe"), "VGPUDriverDiagnostic"])
            check_original()
            state["hashes"]["restored"] = digest(target)
            state["hashes"]["plist_restored"] = digest(plist)
            state["restored_metadata"] = metadata(target)
            state["restored_plist_metadata"] = metadata(plist)
            require(all(item.get("exit_confirmed") is not False
                        for item in state["probes"].values()), "存在退出未确认的探测进程")
            state["restore_verified"] = True
        else:
            old_name = state.get("old_inode_path")
            if old_name:
                old = pathlib.Path(old_name)
                if old.exists():
                    require(old.stat().st_ino == state["original_inode"], "旧 inode 路径已变化")
                    old.unlink()
            for name in state.get("temporary_images", []):
                path = pathlib.Path(name)
                if path.exists():
                    safe_path(path)
                    path.unlink()
            state["not_replaced"] = True
        save()
    elif phase == "report":
        state["status"] = "COMPLETED" if (state.get("experiment_ok") and
            state.get("restore_verified") and int(sys.argv[3]) == 0) else "BLOCKED"
        print(cfg["marker"] + json.dumps(state, sort_keys=True), flush=True)
        sys.exit(0 if state["status"] == "COMPLETED" else 1)
    else:
        raise RuntimeError("未知阶段")
except Exception as exc:
    state[phase + "_error"] = type(exc).__name__ + ": " + str(exc)
    save()
    print("BLOCKED phase=" + phase + " backup=" + state["backup"], file=sys.stderr, flush=True)
    sys.exit(1)
'''


def sha256_arg(value):
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise argparse.ArgumentTypeError("必须提供完整的 64 位 SHA-256")
    return value.lower()


def remote_path_arg(value):
    path = pathlib.PurePosixPath(value)
    if (not value.startswith("/") or value.startswith("//") or ".." in path.parts
            or str(path) != value or any(ord(char) < 32 or ord(char) == 127 for char in value)):
        raise argparse.ArgumentTypeError("必须提供无控制字符、无 .. 的规范远端绝对路径")
    return value


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("candidate", "probe", "trust-probe"):
        ap.add_argument("--" + name, required=True, type=remote_path_arg,
                        help="已存在的远端绝对路径；本工具不上传输入")
        ap.add_argument("--" + name + "-sha256", required=True, type=sha256_arg)
    ap.add_argument("--stage", required=True, choices=STAGES)
    ap.add_argument("--env-file", type=pathlib.Path, default=ROOT / ".env.local")
    ap.add_argument("--out-root", type=pathlib.Path, default=ROOT / "out")
    ap.add_argument("--print-script", action="store_true",
                    help="仅输出待执行脚本，不读取凭据、不连接、不创建结果目录")
    return ap


def make_request(args):
    token = uuid.uuid4().hex
    result = {name: getattr(args, name) for name in (
        "candidate", "candidate_sha256", "probe", "probe_sha256",
        "trust_probe", "trust_probe_sha256", "stage")}
    result.update(target=TARGET, binary_sha256=BINARY_SHA, plist_sha256=PLIST_SHA,
                  token=token, marker="PUBLIC_CGL_RESULT_" + token + "=")
    return result


def build_script(request):
    payload = base64.b64encode(json.dumps(request).encode()).decode()
    worker = base64.b64encode(REMOTE_WORKER.encode()).decode()
    # 密码只经 mssh.run 的 stdin 传入 sudo，绝不进入脚本或请求记录。
    return "\n".join([
        "set -eu", "umask 077", "export PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "unset PYTHONPATH PYTHONHOME", "WORK=''",
        "finish() {",
        "  rc=$?", "  trap - EXIT", "  trap '' INT TERM HUP", "  set +e",
        '  if [ -n "$WORK" ] && [ -f "$WORK/worker.py" ] && [ -f "$WORK/request.json" ]; then',
        '    /usr/bin/python3 -I "$WORK/worker.py" "$WORK" restore',
        "    restore_rc=$?",
        '    if [ "$restore_rc" -ne 0 ]; then rc=1; fi',
        '    /usr/bin/python3 -I "$WORK/worker.py" "$WORK" report "$rc"',
        "    report_rc=$?", '    if [ "$report_rc" -ne 0 ]; then rc=1; fi',
        "  else", "    rc=1", "  fi",
        '  if [ "$rc" -ne 0 ]; then printf "BLOCKED backup=%s\\n" "$WORK/VGPUDriverDiagnostic.bundle" >&2; fi',
        '  exit "$rc"', "}", "trap finish EXIT", "trap 'exit 130' INT",
        "trap 'exit 143' TERM", "trap 'exit 129' HUP",
        '/usr/bin/python3 -I -c ' + shlex.quote("import sys; assert sys.version_info >= (3, 8)"),
        "WORK=$(/usr/bin/mktemp -d /var/tmp/public-cgl-cycle.XXXXXXXX)",
        "printf %s " + shlex.quote(payload) + ' | /usr/bin/base64 -D > "$WORK/request.json"',
        "printf %s " + shlex.quote(worker) + ' | /usr/bin/base64 -D > "$WORK/worker.py"',
        '/usr/bin/python3 -I "$WORK/worker.py" "$WORK" run', "",
    ])


def verified_result(record, request, exit_code):
    probes = record.get("probes", {})
    hashes = record.get("hashes", {})
    return (exit_code == 0 and record.get("status") == "COMPLETED"
            and record.get("restore_verified") is True
            and record.get("experiment_ok") is True
            and record.get("backup_verified") is True
            and isinstance(record.get("original_inode"), int)
            and isinstance(record.get("restoration_inode"), int)
            and record["original_inode"] != record["restoration_inode"]
            and bool(record.get("old_inode_path"))
            and isinstance(record.get("plist_metadata"), dict)
            and hashes.get("before") == BINARY_SHA
            and hashes.get("replaced") == request["candidate_sha256"]
            and hashes.get("restored") == BINARY_SHA
            and all(hashes.get(key) == PLIST_SHA for key in
                    ("plist_before", "plist_replaced", "plist_restored"))
            and record.get("restored_metadata") == {"mode": 0o755, "uid": 0, "gid": 0}
            and record.get("restored_plist_metadata") == record.get("plist_metadata")
            and all(probes.get(name, {}).get("exit_code") == 0
                    and probes[name].get("exit_confirmed") is True
                    and probes[name].get("timeout") is False for name in
                    ("trust_before", "trust_replaced", "public", "trust_restored")))


def main(argv=None):
    args = parser().parse_args(argv)
    request = make_request(args)
    script = build_script(request)
    if args.print_script:
        print(script, end="")
        return 0
    # 延迟导入使本地参数/脚本检查完全不依赖 paramiko 或凭据。
    import mssh

    args.out_root.mkdir(parents=True, exist_ok=True)
    out = pathlib.Path(tempfile.mkdtemp(prefix="public-cgl-cycle-", dir=str(args.out_root)))
    (out / "request.json").write_text(json.dumps(request, indent=2), encoding="utf-8")
    (out / "remote.sh").write_text(script, encoding="utf-8")
    result = {"status": "BLOCKED", "restore_verified": False, "ssh_exit_code": None}
    stdout, stderr = io.StringIO(), io.StringIO()
    client = None
    password = ""
    try:
        mssh.load_env_file(args.env_file)
        password = os.environ.get("VG_PASS", "")
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            client = mssh.connect(connect_timeout=30.0, retries=1)
            # 45 秒后请求 killpg，再等待 5 秒；退出未确认会记录为失败。
            # EXIT trap 无法保证 SIGKILL、主机故障或 SSH 断连后的恢复及证据回传。
            code = mssh.run(client, script, timeout=None, use_sudo=True, password=password)
        result["ssh_exit_code"] = code
        records = [line[len(request["marker"]):] for line in stdout.getvalue().splitlines()
                   if line.startswith(request["marker"])]
        if len(records) != 1:
            raise RuntimeError("缺少唯一的远端恢复证据")
        record = json.loads(records[0])
        result["remote"] = record
        result["restore_verified"] = record.get("restore_verified") is True
        if verified_result(record, request, code):
            result["status"] = "COMPLETED"
        for name in ("trust_before", "trust_replaced", "public", "trust_restored"):
            details = record.get("probes", {}).get(name)
            if details:
                for stream in ("stdout", "stderr"):
                    data = base64.b64decode(details[stream + "_base64"], validate=True)
                    if password:
                        data = data.replace(password.encode(), b"[REDACTED]")
                    (out / (name + "." + stream)).write_bytes(data)
    except (Exception, SystemExit, KeyboardInterrupt) as exc:
        # 不输出可能包含连接凭据的异常正文。
        result["status"] = "BLOCKED"
        result["error_type"] = type(exc).__name__
    finally:
        if client is not None:
            client.close()
        for name, data in (("ssh.stdout", stdout.getvalue()), ("ssh.stderr", stderr.getvalue()),
                           ("result.json", json.dumps(result, indent=2, ensure_ascii=False))):
            if password:
                data = data.replace(password, "[REDACTED]")
            (out / name).write_text(data, encoding="utf-8")
    print(result["status"] + " out=" + str(out))
    if result["status"] != "COMPLETED":
        print("BLOCKED：实验失败或恢复证据不完整；检查 result.json 与远端 backup 路径。", file=sys.stderr)
    return 0 if result["status"] == "COMPLETED" else 1


if __name__ == "__main__":
    sys.exit(main())
