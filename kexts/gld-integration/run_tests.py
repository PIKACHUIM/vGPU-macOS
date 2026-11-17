"""编排真实 SDK CPU 回归；真实设备生命周期须显式启用，不安装、不重启、不绘制。"""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import runpy
import shlex
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RUNNERS = ("integration_read_verify.py", "controlled_integration_probe.py")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", action="store_true",
                        help="CPU 回归后执行原有 --device 生命周期检查（需要 sudo 和已验收后端）")
    parser.add_argument("--gpu-evidence", type=Path,
                        help="已有 multi-resource GPU 验收文本；仅 --device 使用，不重跑 GPU")
    parser.add_argument("--bgra-evidence", type=Path,
                        help="已有 BGRA results.json；仅 --device 使用，不重跑 GPU")
    args = parser.parse_args()
    if args.device and (args.gpu_evidence is None or args.bgra_evidence is None):
        parser.error("--device 必须显式指定 --gpu-evidence 和 --bgra-evidence")
    if not args.device and (args.gpu_evidence is not None or args.bgra_evidence is not None):
        parser.error("验收证据参数仅可与 --device 一起使用")

    evidence_hashes = {}
    if args.device:
        evidence = args.gpu_evidence.read_bytes()
        if "EXIT_CODE=0" not in evidence.decode("utf-8") or "PASS 141 alpha" not in evidence.decode("utf-8"):
            raise RuntimeError("GPU 验收文本缺少成功标记")
        bgra = args.bgra_evidence.read_bytes()
        if json.loads(bgra)["status"] != 0:
            raise RuntimeError("BGRA 验收结果未通过")
        evidence_hashes = {str(args.gpu_evidence.resolve()): hashlib.sha256(evidence).hexdigest(),
                           str(args.bgra_evidence.resolve()): hashlib.sha256(bgra).hexdigest()}

    # 复用维护中的完整构建/CPU fixture 清单，不再维护另一份易过时的上传清单。
    runner_hashes = {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                     for name in RUNNERS}
    spec = importlib.util.spec_from_file_location("mssh", ROOT / "tools/mssh.py")
    mssh = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mssh)
    mssh.load_env_file(mssh.DEFAULT_ENV_FILE)
    out = ROOT / "out"
    out.mkdir(exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    local = out / ("integration-suite-" + stamp)
    local.mkdir()
    log = io.StringIO()
    summary = {"status": "failed", "scope": "CPU+device" if args.device else "CPU",
               "runners": [], "runner_sha256": runner_hashes,
               "evidence_sha256": evidence_hashes,
               "device": "pending" if args.device else "not_requested"}
    client = None

    def run(command, sudo=False):
        log.write(f"COMMAND sudo={sudo} {command}\n")
        start = log.tell()
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            status = mssh.run(client, command, 120, use_sudo=sudo,
                              password=os.environ.get("VG_PASS"))
        output = log.getvalue()[start:]
        log.write(f"EXIT {status}\n")
        if status:
            raise RuntimeError(f"客体操作失败({status})；见 {local / 'validation.log'}")
        return output

    try:
        client = mssh.connect(retries=1)
        before = run("sysctl -n kern.boottime")
        manifests = {}
        controlled = None
        for name in RUNNERS:
            print("RUN " + name, flush=True)
            log.write("RUN " + name + "\n")
            # 必须正常返回并提供源码快照，提前退出不能作为完成整个回归的证据。
            try:
                result = runpy.run_path(str(HERE / name), run_name="__main__")
            except SystemExit as exc:
                raise RuntimeError(f"子入口提前退出：{name}，退出码 {exc.code}") from exc
            child_local = Path(result["local"])
            manifest = json.loads((child_local / "source-sha256.json").read_text(encoding="utf-8"))
            if not manifest:
                raise RuntimeError("子入口没有源码快照：" + name)
            for rel, digest in manifest.items():
                path = ROOT / rel if rel.startswith("kexts/") else ROOT / "kexts" / rel
                key = path.relative_to(ROOT).as_posix()
                if key in manifests and manifests[key] != digest:
                    raise RuntimeError("两个回归使用了不同源码：" + key)
                manifests[key] = digest
            summary["runners"].append({"name": name, "status": "passed", "output": str(child_local)})
            log.write("PASS " + name + " " + str(child_local) + "\n")
            if name == "controlled_integration_probe.py":
                controlled = result

        if args.device:
            boot = run("log show --last boot --style compact --predicate 'eventMessage CONTAINS \"vgpu-fb: start, provider=\"'", sudo=True)
            if "[build multi-resource-v2-unorm8]" not in boot:
                raise RuntimeError("当前启动没有已验收后端 tag，不启用候选")
            run("shasum -a 256 /Library/Extensions/VGPUFramebuffer.kext/Contents/MacOS/vgpuFramebuffer")
            for path, digest in evidence_hashes.items():
                if hashlib.sha256(Path(path).read_bytes()).hexdigest() != digest:
                    raise RuntimeError("验收证据发生变化：" + path)
            remote = controlled["remote"]
            probe = shlex.quote(remote + "/default/integration_probe")
            exe = shlex.quote(remote + "/default/VGPUDriverIntegration.bundle/Contents/MacOS/VGPUDriverIntegration")
            print("RUN 显式授权的真实设备生命周期检查", flush=True)
            run(f"{probe} {exe} --device", sudo=True)
            summary["device"] = "passed"

        if before != run("sysctl -n kern.boottime"):
            raise RuntimeError("回归期间启动时间变化")
        for rel, digest in manifests.items():
            if hashlib.sha256((ROOT / rel).read_bytes()).hexdigest() != digest:
                raise RuntimeError("回归期间源码发生变化：" + rel)
        for name, digest in runner_hashes.items():
            if hashlib.sha256((HERE / name).read_bytes()).hexdigest() != digest:
                raise RuntimeError("回归期间入口发生变化：" + name)
        (local / "source-sha256.json").write_text(json.dumps(manifests, indent=2), encoding="utf-8")
        summary["status"] = "passed"
        scope = "CPU 与设备生命周期" if args.device else "CPU（未执行真实设备生命周期/GPU 验收）"
        message = "PASS " + scope + " " + str(local)
        log.write(message + "\n")
        print(message, flush=True)
    except BaseException as exc:
        log.write(f"FAIL {type(exc).__name__}: {exc}\n")
        print("FAIL 回归未完成；日志 " + str(local / "validation.log"), file=sys.stderr, flush=True)
        raise
    finally:
        (local / "validation.log").write_text(log.getvalue(), encoding="utf-8")
        (local / "results.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
        if client is not None:
            client.close()


if __name__ == "__main__":
    main()
