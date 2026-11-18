#!/usr/bin/env python3
"""Real CPU-only Linux lifecycle controls for the profiler workload runner."""
import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from profile_harness import process


def _death_control_fixture(base, mode):
    """Isolated CPU-only subreaper; never adopts the unittest caller's children."""
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "test PR_SET_CHILD_SUBREAPER")
    ready, grandchild = base / "ready.json", base / "grandchild.json"
    workload = "import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(30)"
    if mode in ("controller-kill", "controller-exit"):
        workload = f"""import os,signal,time,json
signal.signal(signal.SIGTERM,signal.SIG_IGN)
if os.fork()==0:
 os.setsid()
 __import__('pathlib').Path({str(grandchild)!r}).write_text(json.dumps({{'pid':os.getpid()}}))
time.sleep(30)
"""
    code = f"""import json,os,sys
from pathlib import Path
sys.path.insert(0,{str(ROOT / 'scripts')!r})
from profile_harness import process
def observe(snapshot):
 native=snapshot['native']
 if native is not None:
  row={{'native':native,'supervisor':process._identity(native['ppid']),'controller':process._identity(os.getpid())}}
  Path({str(ready)!r}).write_text(json.dumps(row))
  if {mode!r}=='controller-exit' and Path({str(grandchild)!r}).exists(): os._exit(23)
try:
 row=process.run_owned([sys.executable,'-E','-B','-c',{workload!r}],Path({str(ROOT)!r}),dict(os.environ),Path({str(base / 'logs')!r}),10,poll_seconds=.02,observer=observe)
 Path({str(base / 'receipt.json')!r}).write_text(json.dumps(row))
except RuntimeError as exc:
 Path({str(base / 'controller-error.txt')!r}).write_text(str(exc))
"""
    controller = unrelated = None
    tracked = []
    try:
        unrelated = subprocess.Popen([sys.executable, "-E", "-B", "-c", "import time; time.sleep(30)"])
        unrelated_identity = process._identity(unrelated.pid)
        controller = subprocess.Popen([sys.executable, "-E", "-B", "-c", code],
                                      start_new_session=True, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                identities = json.loads(ready.read_text())
                if mode not in ("controller-kill", "controller-exit") or grandchild.exists():
                    break
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(.01)
        else:
            raise AssertionError("death-control workload did not become ready")
        tracked = [identities[name] for name in ("controller", "supervisor", "native")]
        if grandchild.exists():
            row = process._identity(json.loads(grandchild.read_text())["pid"])
            if row is not None:
                tracked.append(row)
        assert identities["supervisor"]["session"] == identities["supervisor"]["pid"]
        assert identities["supervisor"]["session"] != identities["controller"]["session"]
        assert identities["native"]["session"] == identities["native"]["pid"]
        if mode == "controller-kill":
            assert process._signal_owned(identities["controller"], signal.SIGKILL)
        elif mode == "controller-group-term":
            # This process group contains only our controller; the detached
            # supervisor and native sessions must not receive this signal.
            os.killpg(controller.pid, signal.SIGTERM)
        elif mode.startswith("supervisor-"):
            number = signal.SIGKILL if mode.endswith("kill") else signal.SIGTERM
            assert process._signal_owned(identities["supervisor"], number)
        elif mode == "both-kill":
            assert process._signal_owned(identities["controller"], signal.SIGKILL)
            assert process._signal_owned(identities["supervisor"], signal.SIGKILL)
        elif mode != "controller-exit":
            raise AssertionError("unknown death-control mode")
        deadline = time.monotonic() + 7
        remaining = tracked
        while remaining and time.monotonic() < deadline:
            controller.poll()
            remaining = []
            for row in tracked:
                current = process._identity(row["pid"])
                if current is not None and current["start_ticks"] == row["start_ticks"]:
                    if current["state"] == "Z" and current["ppid"] == os.getpid():
                        try:
                            os.waitpid(row["pid"], os.WNOHANG)
                        except ChildProcessError:
                            pass
                    else:
                        remaining.append(current)
            time.sleep(.01)
        assert not remaining, remaining
        assert process._identity(unrelated.pid)["start_ticks"] == unrelated_identity["start_ticks"]
        assert unrelated.poll() is None
        if mode in ("controller-group-term", "supervisor-term"):
            receipt = json.loads((base / "receipt.json").read_text())
            assert not receipt["passed"] and receipt["drain"]["passed"]
        if mode == "supervisor-kill":
            assert "failed without receipt" in (base / "controller-error.txt").read_text()
        print(json.dumps({"mode": mode, "drained": True, "unrelated_survived": True,
                          "tracked": tracked}))
    finally:
        # Even a failed assertion cleans only this isolated scenario's ancestry.
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            rows = process._descendants(os.getpid())
            if not rows:
                break
            for row in rows:
                if row["ppid"] == os.getpid():
                    if row["state"] != "Z":
                        process._signal_owned(row, signal.SIGKILL)
                    try:
                        os.waitpid(row["pid"], os.WNOHANG)
                    except ChildProcessError:
                        pass
            time.sleep(.01)
        if controller is not None:
            controller.poll()
        if unrelated is not None:
            unrelated.poll()


@unittest.skipUnless(sys.platform == "linux", "Linux ownership contract")
class OwnedProcessTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.number = 0

    def run_code(self, code, **kwargs):
        self.number += 1
        return process.run_owned([sys.executable, "-c", code], ROOT, dict(os.environ),
                                 self.base / str(self.number), poll_seconds=0.03,
                                 timeout_seconds=kwargs.pop("timeout_seconds", 2), **kwargs)

    def assert_drained(self, receipt):
        self.assertTrue(receipt["drain"]["passed"], receipt)
        self.assertTrue(receipt["logs_closed"])
        for row in receipt["processes"]:
            current = process._identity(row["pid"])
            self.assertTrue(current is None or current["start_ticks"] != row["start_ticks"], row)

    def test_normal_exit_and_logs(self):
        row = self.run_code("import resource; print(resource.getrlimit(resource.RLIMIT_CORE)); print('err', file=__import__('sys').stderr)",
                            observer=lambda snapshot: {"alive": snapshot["alive_count"]})
        self.assertTrue(row["passed"], row)
        self.assertEqual(row["native_exit_code"], 0)
        self.assertEqual(Path(row["outputs"]["stdout"]["path"]).read_text(), "(0, 0)\n")
        self.assertEqual(Path(row["outputs"]["stderr"]["path"]).read_text(), "err\n")
        self.assertGreater(row["cpu"]["user_seconds"] + row["cpu"]["system_seconds"], 0)
        self.assertEqual(row["native"]["pid"], row["native"]["session"])
        self.assert_drained(row)

    def test_nonzero_exit(self):
        row = self.run_code("raise SystemExit(17)")
        self.assertFalse(row["passed"])
        self.assertEqual(row["native_exit_code"], 17)
        self.assert_drained(row)

    def test_timeout_ignoring_term(self):
        row = self.run_code("import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(20)", timeout_seconds=0.15)
        self.assertTrue(row["timed_out"])
        self.assertEqual(row["native_exit_code"], -signal.SIGKILL)
        self.assertLess(row["elapsed_seconds"], 4)
        self.assert_drained(row)

    def test_grandchild_new_session_and_unrelated_child(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(20)"])
        def cleanup():
            if unrelated.poll() is None:
                unrelated.kill()
            unrelated.wait()
        self.addCleanup(cleanup)
        row = self.run_code("""import os,signal,time
signal.signal(signal.SIGTERM,signal.SIG_IGN)
if os.fork()==0:
 os.setsid()
 if os.fork()==0:
  time.sleep(20)
 else: os._exit(0)
time.sleep(20)
""", timeout_seconds=0.2)
        self.assertTrue(row["timed_out"])
        self.assertGreaterEqual(len(row["processes"]), 2)
        self.assertTrue(any(x["session"] != row["native"]["session"] for x in row["processes"]))
        self.assertIsNone(unrelated.poll())
        self.assert_drained(row)

    def test_controller_sigterm_cancels(self):
        fired = False
        def cancel(snapshot):
            nonlocal fired
            if not fired:
                fired = True
                os.kill(os.getpid(), signal.SIGTERM)
        previous = signal.getsignal(signal.SIGTERM)
        row = self.run_code("import time; time.sleep(20)", observer=cancel)
        self.assertTrue(row["cancelled"], row)
        self.assertFalse(row["passed"])
        self.assertEqual(signal.getsignal(signal.SIGTERM), previous)
        self.assert_drained(row)

    def test_observer_error_cancels(self):
        def fail(snapshot):
            raise ValueError("deliberate callback failure")
        row = self.run_code("import time; time.sleep(20)", observer=fail)
        self.assertIn("observer_error", row["reason"])
        self.assertFalse(row["passed"])
        self.assert_drained(row)

    def test_overflow_bounds_both_streams(self):
        row = self.run_code("import os; os.write(1,b'x'*8192); os.write(2,b'y'*8192)", log_limit_bytes=100)
        self.assertFalse(row["passed"])
        self.assertTrue(any(x["truncated"] for x in row["outputs"].values()))
        for output in row["outputs"].values():
            self.assertLessEqual(Path(output["path"]).stat().st_size, 100)
        self.assert_drained(row)

    def test_invalid_arguments_and_exclusive_output(self):
        for argv in ([], "echo hi", ["\0"]):
            with self.assertRaises(ValueError):
                process.run_owned(argv, ROOT, {}, self.base / "invalid", 1)
        self.assertFalse((self.base / "invalid").exists())
        with self.assertRaises(FileExistsError):
            process.run_owned([sys.executable, "-c", "pass"], ROOT, {}, self.base, 1)

    def test_missing_executable_receipt(self):
        row = process.run_owned(["/does/not/exist"], ROOT, {}, self.base / "missing", 1)
        self.assertFalse(row["passed"])
        self.assertIsNone(row["native_exit_code"])
        self.assertIn("FileNotFoundError", row["error"])
        self.assert_drained(row)

    def test_orphan_is_drained_without_timeout(self):
        row = self.run_code("""import os,signal,time
if os.fork()==0:
 os.setsid()
 signal.signal(signal.SIGTERM,signal.SIG_IGN)
 time.sleep(20)
else:
 time.sleep(.1)
 os._exit(0)
""")
        self.assertEqual(row["native_exit_code"], 0)
        self.assertEqual(row["reason"], "orphan_descendants")
        self.assertFalse(row["passed"])
        self.assert_drained(row)

    def test_observer_base_exception_is_drained(self):
        def fail(snapshot):
            raise SystemExit("deliberate observer exit")
        row = self.run_code("import time; time.sleep(20)", observer=fail)
        self.assertIn("SystemExit", row["reason"])
        self.assert_drained(row)

    def test_empty_positional_argument(self):
        row = process.run_owned([sys.executable, "-c", "import sys; assert sys.argv[1]==''", ""],
                                ROOT, {}, self.base / "empty", 1)
        self.assertTrue(row["passed"], row)

    def test_late_signal_stays_cancellation(self):
        original = process._supervise
        def late_signal(connection, *args):
            original(connection, *args)
            os.kill(os.getppid(), signal.SIGTERM)
        with mock.patch.object(process, "_supervise", late_signal):
            row = self.run_code("pass")
        self.assertTrue(row["cancelled"], row)
        self.assertFalse(row["passed"])
        self.assert_drained(row)

    def test_unrelated_grandchild_is_not_adopted(self):
        unrelated = subprocess.Popen([sys.executable, "-c", """import os,time
if os.fork()==0:
 time.sleep(.2)
 os._exit(19)
time.sleep(.4)
"""])
        def cleanup():
            if unrelated.poll() is None:
                unrelated.kill()
            unrelated.wait()
        self.addCleanup(cleanup)
        row = self.run_code("import time; time.sleep(.6)")
        self.assertTrue(row["passed"], row)
        self.assertNotIn(unrelated.pid, [x["pid"] for x in row["reaped"]])
        self.assertEqual(unrelated.wait(), 0)
        self.assert_drained(row)

    def test_custom_sigchld_rejected_before_spawn(self):
        previous = signal.signal(signal.SIGCHLD, signal.SIG_IGN)
        try:
            with self.assertRaisesRegex(RuntimeError, "SIGCHLD"):
                self.run_code("pass")
        finally:
            signal.signal(signal.SIGCHLD, previous)
        self.assertFalse((self.base / "1").exists())

    def test_pid_reuse_never_signals(self):
        expected = dict(pid=12345, start_ticks=100)
        reused = dict(pid=12345, start_ticks=101)
        with mock.patch.object(process, "_identity", return_value=reused), \
             mock.patch.object(os, "pidfd_open") as opened:
            self.assertFalse(process._signal_owned(expected, signal.SIGTERM))
            opened.assert_not_called()
        with mock.patch.object(process, "_identity", side_effect=[expected, reused]), \
             mock.patch.object(os, "pidfd_open", return_value=99), \
             mock.patch.object(os, "close"), \
             mock.patch.object(signal, "pidfd_send_signal") as sent:
            self.assertFalse(process._signal_owned(expected, signal.SIGKILL))
            sent.assert_not_called()

    def death_control(self, mode):
        completed = subprocess.run([sys.executable, "-E", "-B", __file__,
                                    "--death-control", str(self.base), mode],
                                   cwd=ROOT, capture_output=True, text=True, timeout=20)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertTrue(result["drained"])
        self.assertTrue(result["unrelated_survived"])

    def test_controller_sigkill_drains_new_session_descendant(self):
        self.death_control("controller-kill")

    def test_controller_abrupt_exit_drains_new_session_descendant(self):
        self.death_control("controller-exit")

    def test_controller_group_sigterm_keeps_cleanup_owner(self):
        self.death_control("controller-group-term")

    def test_supervisor_sigterm_drains_native(self):
        self.death_control("supervisor-term")

    def test_supervisor_sigkill_kills_direct_native(self):
        self.death_control("supervisor-kill")

    def test_controller_and_supervisor_sigkill_kills_direct_native(self):
        self.death_control("both-kill")

    def test_parent_death_guard_arms_before_identity_check(self):
        expected = dict(pid=12345, start_ticks=100)
        for current in (None, dict(pid=12346, start_ticks=100, state="S"),
                        dict(pid=12345, start_ticks=101, state="S"),
                        dict(pid=12345, start_ticks=100, state="Z")):
            events = []
            libc = mock.Mock()
            libc.prctl.side_effect = lambda *args: events.append("arm") or 0
            def identity(pid):
                events.append("check")
                return current
            with mock.patch.object(ctypes, "CDLL", return_value=libc), \
                 mock.patch.object(process, "_identity", side_effect=identity):
                with self.assertRaisesRegex(RuntimeError, "parent changed"):
                    process._parent_death_signal(expected, signal.SIGKILL)
            self.assertEqual(events, ["arm", "check"])
            libc.prctl.assert_called_once_with(1, signal.SIGKILL, 0, 0, 0)

    def test_parent_death_prctl_failure_fails_closed(self):
        libc = mock.Mock()
        libc.prctl.return_value = -1
        with mock.patch.object(ctypes, "CDLL", return_value=libc), \
             mock.patch.object(process, "_identity") as identity:
            with self.assertRaisesRegex(OSError, "PR_SET_PDEATHSIG"):
                process._parent_death_signal(dict(pid=12345, start_ticks=100), signal.SIGTERM)
            identity.assert_not_called()


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--death-control":
        _death_control_fixture(Path(sys.argv[2]), sys.argv[3])
    else:
        unittest.main()
