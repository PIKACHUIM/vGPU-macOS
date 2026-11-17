"""本地故障注入：真实临时文件恢复；macOS 命令及进程等待以 mock 替代。"""
import ast
import base64
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "public_cgl_cycle", pathlib.Path(__file__).with_name("public-cgl-cycle.py"))
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
TREE = ast.parse(RUNNER.REMOTE_WORKER)
MAIN = next(i for i, node in enumerate(TREE.body) if isinstance(node, ast.Try))
SETUP = compile(ast.Module(body=TREE.body[:MAIN], type_ignores=[]), "worker", "exec")
ACTION = compile(ast.Module(body=TREE.body[MAIN:], type_ignores=[]), "worker", "exec")


class CycleTests(unittest.TestCase):
    def setUp(self):
        if os.name == "nt":
            # Windows 无 SIGKILL，且只读 fd 不支持 fsync；仅适配这两处平台差异。
            patch = mock.patch.object(signal, "SIGKILL", 9, create=True)
            patch.start()
            self.addCleanup(patch.stop)
            original_fsync = os.fsync
            def windows_fsync(fd):
                try:
                    original_fsync(fd)
                except OSError as exc:
                    if exc.errno != 9:
                        raise
            patch = mock.patch.object(os, "fsync", windows_fsync)
            patch.start()
            self.addCleanup(patch.stop)
        self.temp = tempfile.TemporaryDirectory(prefix="public-cgl-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name).resolve()
        self.work = self.root / "work"
        self.work.mkdir()
        self.bundle = self.root / "Fixture.bundle"
        self.target = self.bundle / "Contents/MacOS/fixture"
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(b"original fixture")
        self.plist = self.bundle / "Contents/Info.plist"
        self.plist.write_bytes(b"plist fixture")
        self.trust = self.work / "trust_probe"
        self.trust.write_bytes(b"mock trust")
        self.cfg = {"target": str(self.target), "token": "fixture-token",
                    "binary_sha256": self.sha(self.target),
                    "plist_sha256": self.sha(self.plist),
                    "trust_probe_sha256": self.sha(self.trust), "marker": "TEST="}
        (self.work / "request.json").write_text(json.dumps(self.cfg))
        self.ns = self.load()
        self.state = self.ns["state"]
        self.state.update(original_metadata=self.ns["metadata"](self.target),
                          plist_metadata=self.ns["metadata"](self.plist),
                          original_inode=self.target.stat().st_ino,
                          original_manifest=self.portable_manifest(self.bundle),
                          backup_verified=True, replacement_intent=True)
        shutil.copytree(self.bundle, self.state["backup"])
        self.old = self.target.parent / (".public-cgl-old-" + self.cfg["token"])
        os.link(self.target, self.old)
        self.state["old_inode_path"] = str(self.old)
        self.state["old_link_path"] = str(self.old)
        candidate = self.root / "candidate"
        candidate.write_bytes(b"candidate fixture")
        os.replace(candidate, self.target)
        self.ns["save"]()

    @staticmethod
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def portable_manifest(self, root):
        # Windows 不提供 macOS flags/xattr/ACL；只在恢复测试替代此系统边界。
        result = {}
        for path in [root] + sorted(root.rglob("*")):
            item = self.ns["metadata"](path)
            item.update(kind="file" if path.is_file() else "directory", flags=0, acl=[])
            if path.is_file():
                item["sha256"] = self.sha(path)
            result[str(path.relative_to(root))] = item
        return result

    def command(self, argv):
        if argv[0] == "/bin/cp":
            shutil.copy2(argv[-2], argv[-1])
        elif argv[0] == "/bin/mv":
            os.replace(argv[-2], argv[-1])
        elif argv[0] == "/bin/chmod":
            os.chmod(argv[-1], int(argv[-2], 8))
        elif argv[0] != "/usr/sbin/chown":
            raise AssertionError(argv)

    def load(self):
        namespace = {}
        with mock.patch.object(sys, "argv", ["worker", str(self.work), "restore"]):
            exec(SETUP, namespace)
        namespace["manifest"] = self.portable_manifest
        namespace["command"] = self.command
        namespace["probe"] = lambda name, argv: None
        return namespace

    def restore(self, fail=False):
        if fail:
            with self.assertRaises(SystemExit) as caught:
                exec(ACTION, self.ns)
            self.assertEqual(caught.exception.code, 1)
            self.assertFalse(self.disk_state()["restore_verified"])
        else:
            exec(ACTION, self.ns)
            self.assertTrue(self.disk_state()["restore_verified"])
            self.assertEqual(self.sha(self.target), self.cfg["binary_sha256"])
            self.assertEqual(self.portable_manifest(self.bundle), self.state["original_manifest"])
            self.assertNotEqual(self.target.stat().st_ino, self.state["original_inode"])

    def disk_state(self):
        return json.loads((self.work / "state.json").read_text())

    def reload(self):
        self.ns = self.load()
        self.state = self.ns["state"]

    def test_restore_reentry_after_success(self):
        self.restore()
        self.reload()
        self.restore()
        self.assertFalse(self.old.exists())
        self.assertEqual((self.work / "original.inode").stat().st_ino,
                         self.state["original_inode"])

    def test_archive_moved_before_state_saved(self):
        os.replace(self.old, self.work / "original.inode")
        self.reload()
        self.restore()
        self.reload()
        self.restore()

    def test_failure_after_archive_state_save_then_retry(self):
        original_unlink = pathlib.Path.unlink
        def fail_old(path, *args, **kwargs):
            if path == self.old:
                raise OSError("注入旧链接删除失败")
            return original_unlink(path, *args, **kwargs)
        with mock.patch.object(pathlib.Path, "unlink", fail_old):
            self.restore(fail=True)
        self.assertTrue(self.old.exists())
        self.assertEqual(self.disk_state()["old_inode_path"], str(self.work / "original.inode"))
        self.reload()
        self.restore()
        self.assertNotIn("restore_error", self.disk_state())

    def test_trust_failure_after_archive_then_retry(self):
        self.ns["probe"] = mock.Mock(side_effect=RuntimeError("注入 trust 失败"))
        self.restore(fail=True)
        self.assertFalse(self.old.exists())
        self.reload()
        self.restore()

    def test_failed_retry_revokes_previous_success(self):
        self.restore()
        self.reload()
        backup_file = pathlib.Path(self.state["backup"]) / self.target.relative_to(self.bundle)
        backup_file.write_bytes(b"corrupt")
        self.restore(fail=True)
        self.assertIn("恢复备份已变化", self.disk_state()["restore_error"])

    def test_missing_old_inode_fails(self):
        self.old.unlink()
        self.restore(fail=True)

    def test_unconfirmed_process_blocks_restore_verification(self):
        self.state["probes"]["public"] = {"pid": 123, "exit_confirmed": False}
        self.ns["save"]()
        self.restore(fail=True)
        self.assertIn("退出未确认", self.disk_state()["restore_error"])

    def probe_namespace(self):
        namespace = {}
        with mock.patch.object(sys, "argv", ["worker", str(self.work), "restore"]):
            exec(SETUP, namespace)
        return namespace

    def run_probe(self, waits, kill_error=None):
        ns = self.probe_namespace()
        process = mock.Mock(pid=12345)
        process.wait.side_effect = waits
        def popen(*args, **kwargs):
            kwargs["stdout"].write(b"partial stdout")
            kwargs["stdout"].flush()
            kwargs["stderr"].write(b"partial stderr")
            kwargs["stderr"].flush()
            return process
        with mock.patch.object(subprocess, "Popen", side_effect=popen), \
             mock.patch.object(os, "killpg", create=True, side_effect=kill_error) as kill:
            with self.assertRaises((RuntimeError, KeyboardInterrupt)):
                ns["probe"]("public", ["fixture"])
        record = self.disk_state()["probes"]["public"]
        self.assertEqual(record["pid"], 12345)
        self.assertEqual(base64.b64decode(record["stdout_base64"]), b"partial stdout")
        self.assertEqual(base64.b64decode(record["stderr_base64"]), b"partial stderr")
        kill.assert_called_once()
        self.assertEqual(process.wait.call_args_list[-1], mock.call(timeout=5))
        return record

    def test_kill_wait_timeout_persists_evidence(self):
        record = self.run_probe([subprocess.TimeoutExpired("fixture", 45),
                                 subprocess.TimeoutExpired("fixture", 5)])
        self.assertTrue(record["timeout"])
        self.assertTrue(record["termination_wait_timeout"])
        self.assertFalse(record["exit_confirmed"])
        self.assertIsNone(record["exit_code"])
        ns = self.probe_namespace()
        with self.assertRaisesRegex(RuntimeError, "保留原证据"):
            ns["probe"]("public", ["fixture"])
        self.assertEqual(record, self.disk_state()["probes"]["public"])

    def test_timeout_kill_confirmed_remains_failure(self):
        record = self.run_probe([subprocess.TimeoutExpired("fixture", 45), -9])
        self.assertTrue(record["exit_confirmed"])
        self.assertTrue(record["timeout"])
        self.assertEqual(record["exit_code"], -9)

    def test_kill_error_persists_evidence(self):
        record = self.run_probe([subprocess.TimeoutExpired("fixture", 45),
                                 subprocess.TimeoutExpired("fixture", 5)],
                                PermissionError("注入 killpg 失败"))
        self.assertIn("PermissionError", record["killpg_error"])
        self.assertFalse(record["exit_confirmed"])

    def test_interruption_persists_evidence(self):
        record = self.run_probe([KeyboardInterrupt(), subprocess.TimeoutExpired("fixture", 5)])
        self.assertFalse(record["exit_confirmed"])
        self.assertIsNone(record["exit_code"])

    def test_real_local_probe_success(self):
        ns = self.probe_namespace()
        ns["probe"]("local", [sys.executable, "-c",
                              "import sys; print('out'); print('err', file=sys.stderr)"])
        record = self.disk_state()["probes"]["local"]
        self.assertTrue(record["exit_confirmed"])
        self.assertFalse(record["timeout"])
        self.assertEqual(record["exit_code"], 0)
        self.assertGreater(record["pid"], 0)
        self.assertEqual(base64.b64decode(record["stdout_base64"]).strip(), b"out")
        self.assertEqual(base64.b64decode(record["stderr_base64"]).strip(), b"err")

    def test_acl_capture_and_rejection(self):
        acl = self.ns["acl_entries"]
        with mock.patch.object(subprocess, "check_output", return_value=
                               b"-rwxr-xr-x  1 0 0 5 Jan 1 00:00 fixture\n"):
            self.assertEqual(acl(self.target), [])
        with mock.patch.object(subprocess, "check_output", return_value=
                               b"-rwxr-xr-x+ 1 0 0 5 Jan 1 00:00 fixture\n 0: user:ABC allow read\n"):
            self.assertEqual(acl(self.target), ["user:ABC allow read"])
        for output in (b"-rwxr-xr-x+ 1 0 0 5 fixture\n", b"unexpected\n",
                       b"-rwxr-xr-x  1 0 0 5 fixture\n 2: broken\n"):
            with mock.patch.object(subprocess, "check_output", return_value=output):
                with self.assertRaises(RuntimeError):
                    acl(self.target)
        with mock.patch.object(subprocess, "check_output", side_effect=OSError("ACL 读取失败")):
            with self.assertRaises(OSError):
                acl(self.target)

    def test_acl_mismatch_rejects_backup(self):
        original = {".": {"kind": "directory", "flags": 0, "acl": ["user:ABC allow read"]}}
        observed = {".": {"kind": "directory", "flags": 0, "acl": []}}
        self.assertFalse(self.ns["backup_matches"](observed, original))


class StageTests(unittest.TestCase):
    RESULTS = {
        "choose-only": "PUBLIC_CGL_CHOOSE PASS mode=no-draw",
        "context-only": "PUBLIC_CGL_CONTEXT PASS mode=no-draw",
        "clear-read": "PUBLIC_CGL_CLEAR_READ PASS mode=no-draw",
        "gpu": "PUBLIC_CGL_TRIANGLE PASS mode=restricted-red",
        "reject-color": "PUBLIC_CGL_TRIANGLE PASS mode=restricted-reject-color",
        "pbuffer-choose-only": "PUBLIC_CGL_PBUFFER_CHOOSE PASS mode=no-draw",
        "pbuffer-context-only": "PUBLIC_CGL_PBUFFER_CONTEXT PASS mode=no-draw",
        "pbuffer-clear-read": "PUBLIC_CGL_PBUFFER_CLEAR_READ PASS mode=no-draw",
    }

    def request(self, stage):
        argv = ["--stage", stage]
        for name in ("candidate", "probe", "trust-probe"):
            argv.extend(["--" + name, "/tmp/fixture-" + name,
                         "--" + name + "-sha256", "a" * 64])
        return RUNNER.make_request(RUNNER.parser().parse_args(argv))

    def record(self, stage, output):
        probes = {name: {"exit_code": 0, "exit_confirmed": True, "timeout": False}
                  for name in ("trust_before", "trust_replaced", "public", "trust_restored")}
        probes["public"].update(argv=["/tmp/fixture-probe", "--" + stage],
                                stdout_base64=base64.b64encode(output).decode())
        return {
            "status": "COMPLETED", "restore_verified": True, "experiment_ok": True,
            "backup_verified": True, "original_inode": 1, "restoration_inode": 2,
            "old_inode_path": "/tmp/fixture-original", "plist_metadata": {"mode": 420},
            "restored_plist_metadata": {"mode": 420},
            "restored_metadata": {"mode": 0o755, "uid": 0, "gid": 0},
            "hashes": {"before": RUNNER.BINARY_SHA, "replaced": "a" * 64,
                       "restored": RUNNER.BINARY_SHA, "plist_before": RUNNER.PLIST_SHA,
                       "plist_replaced": RUNNER.PLIST_SHA, "plist_restored": RUNNER.PLIST_SHA},
            "probes": probes,
        }

    def test_all_stages_require_their_own_summary(self):
        self.assertEqual(set(RUNNER.STAGES), set(self.RESULTS))
        for stage, expected in self.RESULTS.items():
            request = self.request(stage)
            self.assertEqual(request["expected_public_result"], expected)
            for actual in self.RESULTS.values():
                with self.subTest(stage=stage, actual=actual):
                    output = ("CPU_ORACLE PASS\n" + actual + "\n").encode()
                    self.assertEqual(RUNNER.verified_result(self.record(stage, output), request, 0),
                                     actual == expected)
            for output in (b"", b"CPU_PBUFFER_ORACLE PASS\nCPU_ORACLE PASS\n",
                           expected.replace(" PASS ", " FAIL ").encode(),
                           (expected + "\n" + expected).encode(),
                           (expected + " extra").encode()):
                with self.subTest(stage=stage, output=output):
                    self.assertFalse(RUNNER.verified_result(self.record(stage, output), request, 0))
            record = self.record(stage, expected.encode())
            record["probes"]["public"]["argv"][-1] = "--oracle-only"
            self.assertFalse(RUNNER.verified_result(record, request, 0))

    def test_stage_success_preserves_failure_gates(self):
        stage = "pbuffer-clear-read"
        request = self.request(stage)
        for name, changes in (
                ("public", {"exit_code": 1}), ("public", {"timeout": True}),
                ("public", {"exit_confirmed": False}), ("trust_restored", {"exit_code": 1})):
            with self.subTest(name=name, changes=changes):
                record = self.record(stage, self.RESULTS[stage].encode())
                record["probes"][name].update(changes)
                self.assertFalse(RUNNER.verified_result(record, request, 0))
        for field in ("restore_verified", "experiment_ok", "backup_verified"):
            record = self.record(stage, self.RESULTS[stage].encode())
            record[field] = False
            self.assertFalse(RUNNER.verified_result(record, request, 0))
        record = self.record(stage, self.RESULTS[stage].encode())
        self.assertFalse(RUNNER.verified_result(record, request, 1))
        record["hashes"]["restored"] = "0" * 64
        self.assertFalse(RUNNER.verified_result(record, request, 0))

    def test_remote_stage_check_before_experiment_success(self):
        # 仅执行公开 probe 的 mock 调用、输出判断和成功赋值，绝不进入远端替换流程。
        body = TREE.body[MAIN].body[0].body
        start = next(i for i, node in enumerate(body) if isinstance(node, ast.Expr)
                     and isinstance(node.value, ast.Call)
                     and isinstance(node.value.func, ast.Name) and node.value.func.id == "probe"
                     and node.value.args[0].value == "public")
        block = compile(ast.Module(body=body[start:start + 4], type_ignores=[]), "stage", "exec")
        def require(condition, message):
            if not condition:
                raise RuntimeError(message)
        with tempfile.TemporaryDirectory(prefix="public-cgl-stage-test-") as temp:
            work = pathlib.Path(temp)
            for stage, expected in self.RESULTS.items():
                for output in (expected, "CPU_ORACLE PASS", self.RESULTS["gpu"],
                               expected.replace(" PASS ", " FAIL ")):
                    with self.subTest(stage=stage, output=output):
                        (work / "public.stdout").write_bytes((output + "\n").encode())
                        probe = mock.Mock()
                        state = {}
                        ns = {"cfg": self.request(stage), "work": work, "state": state,
                              "probe": probe, "require": require}
                        if output == expected:
                            exec(block, ns)
                            self.assertTrue(state["experiment_ok"])
                        else:
                            with self.assertRaisesRegex(RuntimeError, "阶段结果不匹配"):
                                exec(block, ns)
                            self.assertNotIn("experiment_ok", state)
                        probe.assert_called_once_with("public", [str(work / "probe"), "--" + stage])


if __name__ == "__main__":
    unittest.main(verbosity=2)
