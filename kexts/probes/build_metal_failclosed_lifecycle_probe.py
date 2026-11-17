"""固定候选快照，在独立 macOS 进程中验证负向生命周期；不安装、不执行 GPU。"""
from datetime import datetime, timezone
from pathlib import Path
import contextlib
import hashlib
import io
import json
import re
import shlex
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import mssh

CANDIDATE_SHA256 = "83eabdcfe91bf20d98165cb9578fc1c99e6d38c4b4c7bff0e8026b89d30305fb"
METAL_UUID = "0CFB909C-C488-3186-AE3B-47E504BA1D78"
DISCOVERY_SHA256 = "b43f72c1aa12fbb98b5ab2b4ebda6377bb212ccf215cc8ba734d76596a3b2381"


def main():
    if len(sys.argv) != 1:
        print("用法：build_metal_failclosed_lifecycle_probe.py", file=sys.stderr)
        return 2
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    local = ROOT / "out" / ("metal-failclosed-lifecycle-" + stamp)
    if not local.parent.is_dir():
        raise RuntimeError("out 父目录不存在")
    local.mkdir()
    remote = "/var/tmp/vgpu-metal-failclosed-lifecycle-" + stamp
    files = {
        "metal_failclosed_lifecycle_probe.m": ROOT / "kexts/probes/metal_failclosed_lifecycle_probe.m",
        "build_metal_failclosed_lifecycle_probe.py": Path(__file__).resolve(),
        "metal-failclosed-candidate.m": ROOT / "out/metal-failclosed-candidate.m",
        "metal-failclosed-contract.txt": ROOT / "out/metal-failclosed-contract.txt",
        "metal-base-evidence-v2.txt": ROOT / "out/metal-base-evidence-v2.txt",
        "metal-base-runtime.txt": ROOT / "out/metal-base-runtime.txt",
    }
    manifest, uploads, binaries, runs = {}, {}, {}, []
    client, status, failure = None, 1, None
    with (local / "build.log").open("w", encoding="utf-8", buffering=1) as log:
        def record(text):
            print(text, flush=True)
            log.write(text + "\n")

        def run(command, expected=0):
            record("COMMAND " + command)
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
                rc = mssh.run(client, command, 120)
            text = capture.getvalue()
            record(text)
            record("EXIT " + str(rc))
            if expected is not None and rc != expected:
                raise RuntimeError("远端命令退出码不符合预期，保留现场")
            return rc, text

        def require(condition, message):
            if not condition:
                raise RuntimeError(message)

        try:
            record("LOCAL " + str(local) + "\nREMOTE " + remote)
            for name, source in files.items():
                data = source.read_bytes()
                (local / name).write_bytes(data)
                manifest[name] = hashlib.sha256(data).hexdigest()
            (local / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
            require(manifest["metal-failclosed-candidate.m"] == CANDIDATE_SHA256,
                    "原候选 SHA256 不匹配契约")
            mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
            client = mssh.connect(retries=1)
            _, version = run("sw_vers -productVersion && sw_vers -buildVersion && uname -m")
            require(version.split() == ["15.7.3", "24G419", "x86_64"], "系统版本不匹配契约")
            _, identity = run("xcrun dyld_info -uuid /System/Library/Frameworks/Metal.framework/Versions/A/Metal")
            require(METAL_UUID in identity, "Metal UUID 不匹配契约")
            run("xcrun --show-sdk-path && xcrun clang --version")
            run("test -d /var/tmp && mkdir " + shlex.quote(remote))
            with client.open_sftp() as sftp:
                for name in files:
                    sftp.put(str(local / name), remote + "/" + name)
                    with sftp.open(remote + "/" + name, "rb") as stream:
                        uploads[name] = hashlib.sha256(stream.read()).hexdigest()
                    require(uploads[name] == manifest[name], "上传 SHA256 不匹配：" + name)
            (local / "upload-sha256.json").write_text(json.dumps(uploads, indent=2), encoding="utf-8")
            prefix = "cd " + shlex.quote(remote) + " && "
            flags = "-std=c11 -arch x86_64 -O1 -g -Wall -Wextra -Werror -fno-objc-arc -Wl,-fatal_warnings"
            sources = "metal_failclosed_lifecycle_probe.m -framework Foundation -framework Metal"
            variants = ("metal-lifecycle-mrc", "metal-lifecycle-asan-ubsan")
            for name in variants:
                sanitizer = (" -fsanitize=address,undefined -fno-omit-frame-pointer"
                             if name.endswith("asan-ubsan") else "")
                run(prefix + f"xcrun clang {flags}{sanitizer} {sources} -o {name}")
                _, imports = run(prefix + "xcrun dyld_info -imports " + name)
                require(not re.search(r"MTLCopyAllDevices|MTLCreateSystemDefaultDevice|IOServiceOpen|IOConnectCall", imports),
                        "发现不允许的设备操作导入")
                (local / (name + "-imports.txt")).write_text(imports, encoding="utf-8")
            with client.open_sftp() as sftp:
                for name in variants:
                    sftp.get(remote + "/" + name, str(local / name))
                    binaries[name] = hashlib.sha256((local / name).read_bytes()).hexdigest()
                    with sftp.open(remote + "/" + name, "rb") as stream:
                        require(hashlib.sha256(stream.read()).hexdigest() == binaries[name],
                                "二进制下载 SHA256 不匹配")
            run(prefix + "shasum -a 256 " + " ".join(files) + " " + " ".join(variants))
            for name in variants:
                environment = ("ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "
                               "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "
                               if name.endswith("asan-ubsan") else "")
                for repetition in range(1, 3):
                    log_name = name + "-run-" + str(repetition) + ".log"
                    command = prefix + environment + "./" + name + " --negative-lifecycle-only"
                    rc, text = run(command, expected=None)
                    (local / log_name).write_text(text, encoding="utf-8")
                    entries = text.count("VGPU_METAL_DIAGNOSTIC_INIT service=0x0 result=nil GPU_NOT_EXECUTED")
                    returns = re.findall(r"^RETURN iteration=(\d+) nil=1$", text, re.MULTILINE)
                    allocations = re.findall(r"^ALLOC iteration=(\d+) size=(\d+) pid=(\d+)$", text, re.MULTILINE)
                    passed = (rc == 0 and entries == 4 and returns == ["1", "2", "3", "4"]
                              and len(allocations) == 4 and all(int(row[1]) >= 656 for row in allocations)
                              and "LIFECYCLE_PASS iterations=4 nil_returns=4 GPU_NOT_EXECUTED" in text
                              and not re.search(r"ERROR: AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer", text))
                    runs.append({"binary": name, "repetition": repetition, "exit": rc,
                                 "entries": entries, "nil_returns": len(returns),
                                 "allocations": allocations, "passed": passed, "log": log_name})
                    require(passed, "生命周期验证失败；停止后续执行并保留证据")
            discovery = "/var/tmp/vgpu-fb/metal-discovery-mrc"
            _, digest = run("shasum -a 256 " + discovery)
            require(digest.split()[0] == DISCOVERY_SHA256, "现成 discovery 二进制身份发生变化")
            rc, baseline = run(discovery + " --discovery-contract-only", expected=None)
            (local / "discovery-contract-only.log").write_text(baseline, encoding="utf-8")
            record("DISCOVERY_EXIT " + str(rc))
            changed = [name for name, source in files.items()
                       if hashlib.sha256(source.read_bytes()).hexdigest() != manifest[name]]
            record("WORKSPACE_CHANGED " + json.dumps(changed))
            require(not changed, "验证期间源文件改变；结果仅对应固定快照")
            status = 0
        except (Exception, SystemExit) as exc:
            # 不输出 SSH 异常细节，避免连接参数或凭据进入日志。
            failure = str(exc) if type(exc) is RuntimeError else type(exc).__name__
            record("FAILED " + failure)
        finally:
            (local / "result.json").write_text(json.dumps({
                "status": status, "failure": failure, "remote": remote,
                "manifest": manifest, "upload_sha256": uploads, "binary_sha256": binaries,
                "runs": runs, "gpu_executed": False, "installed": False,
                "standard_loader_verified": False, "usable_metal_verified": False,
                "limitations": "仅固定系统的直接调用负向生命周期；ASan/UBSan 不覆盖 Apple 系统库全部内部操作，未启用泄漏检测。",
            }, ensure_ascii=False, indent=2), encoding="utf-8")
            record("FINAL " + str(status) + " " + str(local))
            if client:
                client.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
