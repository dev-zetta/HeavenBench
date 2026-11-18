"""Bounded Linux workload ownership. Logs are mutable work files, not sealed evidence.

The synchronous supervisor is a private subreaper, so unrelated caller children
can never become cleanup targets. Observer callbacks must return promptly.
Linux parent-death signals cover controller loss and the direct workload if the
supervisor dies; arbitrary grandchildren still need a living supervisor to drain.
"""
from __future__ import annotations

import ctypes
from datetime import datetime, timezone
import json
import math
import multiprocessing
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import threading
import time

if sys.platform == "linux":
    import resource


def _identity(pid):
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
        fields = text[text.rfind(")") + 2:].split()
        return dict(pid=pid, state=fields[0], ppid=int(fields[1]),
                    pgid=int(fields[2]), session=int(fields[3]), start_ticks=int(fields[19]))
    except (FileNotFoundError, ProcessLookupError):
        return None


def _signal_owned(expected, number):
    """The pidfd pins the target even if it exits between validation and send."""
    try:
        current = _identity(expected["pid"])
        if not current or current["start_ticks"] != expected["start_ticks"]:
            return False
        fd = os.pidfd_open(expected["pid"])
    except (OSError, ValueError, IndexError):
        return False
    try:
        current = _identity(expected["pid"])
        if not current or current["start_ticks"] != expected["start_ticks"]:
            return False
        signal.pidfd_send_signal(fd, number)
        return True
    except (OSError, ValueError, IndexError):
        return False
    finally:
        os.close(fd)


def _descendants(root, errors=None):
    table = {}
    try:
        entries = list(Path("/proc").iterdir())
    except OSError as exc:
        if errors is not None:
            errors.append(f"proc: {type(exc).__name__}: {exc}")
        return []
    for entry in entries:
        if entry.name.isdecimal():
            try:
                row = _identity(int(entry.name))
                if row:
                    table[row["pid"]] = row
            except (OSError, ValueError, IndexError) as exc:
                if errors is not None:
                    errors.append(f"{entry.name}: {type(exc).__name__}: {exc}")
    owned = {root}
    while True:
        more = {pid for pid, row in table.items() if row["ppid"] in owned}
        if more <= owned:
            return [row for pid, row in table.items() if pid in owned and pid != root]
        owned |= more


def _no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def _parent_death_signal(expected_parent, number):
    """Arm before checking: a parent already lost before prctl sends no signal.

    The controller and supervisor are single-threaded. Linux watches the forking
    thread, not every thread in an arbitrary parent process. This guard survives
    ordinary exec, but privileged set-ID/capability-changing exec is outside the
    runner's workload contract. It is cleared on fork, so it is not a tree kill.
    """
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, number, 0, 0, 0) != 0:  # PR_SET_PDEATHSIG
        raise OSError(ctypes.get_errno(), "PR_SET_PDEATHSIG")
    current = _identity(os.getppid())
    if (current is None or current["pid"] != expected_parent["pid"]
            or current["start_ticks"] != expected_parent["start_ticks"]
            or current["state"] == "Z"):
        raise RuntimeError("owned parent changed before death guard was armed")


def _native_setup(expected_parent):
    _parent_death_signal(expected_parent, signal.SIGKILL)
    _no_core()


def _supervise(connection, unused_connection, argv, cwd, env, outdir, timeout, poll, limit,
               expected_parent):
    unused_connection.close()
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    started = time.monotonic_ns()
    receipt = dict(schema_version=1, argv=argv, cwd=str(cwd),
                   started_utc=datetime.now(timezone.utc).isoformat(), started_monotonic_ns=started,
                   controller=_identity(os.getpid()), native=None, native_exit_code=None,
                   reason=None, error=None, signals=[], processes=[], reaped=[], outputs={})
    seen, term_sent, kill_sent = {}, set(), set()
    streams = selectors.DefaultSelector()
    files, process = [], None
    requested = []
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, lambda signum, frame: requested.append(f"signal:{signum}"))
    cleanup_at = None
    next_observer = next_scan = 0.0
    remaining, scan_errors = [], []
    try:
        # Shell/process-group termination must not kill the cleanup owner along
        # with its caller. Parent loss instead requests its normal bounded drain.
        os.setsid()
        receipt["controller"] = _identity(os.getpid())
        _parent_death_signal(expected_parent, signal.SIGTERM)
        _no_core()
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
            raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER")
        for name in ("stdout", "stderr"):
            handle = (outdir / f"{name}.log").open("xb", buffering=0)
            files.append(handle)
            receipt["outputs"][name] = dict(path=str(outdir / f"{name}.log"), bytes=0,
                                             discarded_bytes=0, truncated=False, eof=False)
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True,
                                   preexec_fn=lambda: _native_setup(receipt["controller"]))
        receipt["native"] = _identity(process.pid)
        for name, pipe, handle in zip(("stdout", "stderr"), (process.stdout, process.stderr), files):
            os.set_blocking(pipe.fileno(), False)
            streams.register(pipe, selectors.EVENT_READ, (name, handle))

        while True:
            now = time.monotonic()
            if connection.poll():
                requested.append(connection.recv())
            if requested and receipt["reason"] is None:
                receipt["reason"] = requested[0]
            if now - started / 1e9 >= timeout and receipt["reason"] is None:
                receipt["reason"] = "timeout"
            if now >= next_scan or receipt["reason"] or process.returncode is not None:
                remaining = _descendants(os.getpid(), scan_errors)
                next_scan = now + poll
            for row in remaining:
                key = (row["pid"], row["start_ticks"])
                seen.setdefault(key, row)
                if len(seen) > 4096:
                    receipt["reason"] = receipt["reason"] or "process_limit"
                if row["ppid"] == os.getpid():
                    try:
                        pid, status, usage = os.wait4(row["pid"], os.WNOHANG)
                    except ChildProcessError:
                        continue
                    if pid:
                        code = os.waitstatus_to_exitcode(status)
                        receipt["reaped"].append(dict(pid=pid, start_ticks=row["start_ticks"],
                            exit_code=code, user_seconds=usage.ru_utime, system_seconds=usage.ru_stime))
                        if (pid == process.pid and receipt["native"]
                                and row["start_ticks"] == receipt["native"]["start_ticks"]):
                            process.returncode = code
                            receipt["native_exit_code"] = code
            # Fresh ancestry catches adopted or newly forked descendants on the next tick.
            remaining = [row for row in remaining if Path(f"/proc/{row['pid']}").exists()]
            if scan_errors:
                receipt["reason"] = receipt["reason"] or "process_scan_error"
            if process.returncode is not None and remaining and receipt["reason"] is None:
                receipt["reason"] = "orphan_descendants"
            if receipt["reason"]:
                cleanup_at = cleanup_at or now
                for row in remaining:
                    key = (row["pid"], row["start_ticks"])
                    number = signal.SIGKILL if now - cleanup_at >= 1 else signal.SIGTERM
                    sent = kill_sent if number == signal.SIGKILL else term_sent
                    # Kill direct children only; subreaping adopts every deeper survivor.
                    if key not in sent and row["state"] != "Z" and row["ppid"] == os.getpid():
                        if _signal_owned(row, number):
                            receipt["signals"].append(dict(pid=row["pid"], start_ticks=row["start_ticks"], signal=number))
                        sent.add(key)
                if now - cleanup_at > 5:
                    receipt["error"] = "cleanup_deadline"
                    break
            if process.returncode is not None and not remaining and not streams.get_map():
                remaining = _descendants(os.getpid(), scan_errors)
                if not remaining:
                    break
                continue
            if now >= next_observer:
                connection.send(dict(kind="progress", value=dict(monotonic_ns=time.monotonic_ns(),
                    elapsed_seconds=now - started / 1e9, native=receipt["native"],
                    alive_count=len(remaining), outputs=receipt["outputs"])))
                next_observer = now + poll
            for key, _ in streams.select(min(0.05, poll)):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    receipt["outputs"][key.data[0]]["eof"] = True
                    streams.unregister(key.fileobj)
                    key.fileobj.close()
                    continue
                name, handle = key.data
                output = receipt["outputs"][name]
                keep = data[:max(0, limit - output["bytes"])]
                handle.write(keep)
                output["bytes"] += len(keep)
                output["discarded_bytes"] += len(data) - len(keep)
                if len(keep) != len(data):
                    output["truncated"] = True
                    receipt["reason"] = receipt["reason"] or f"{name}_overflow"
    except BaseException as exc:
        receipt["reason"] = receipt["reason"] or "supervisor_error"
        receipt["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        # Exceptions, including observer transport failure, must also drain the closed ownership tree.
        deadline = time.monotonic() + 5
        while True:
            remaining = _descendants(os.getpid(), scan_errors)
            if not remaining or time.monotonic() >= deadline:
                break
            for row in remaining:
                seen.setdefault((row["pid"], row["start_ticks"]), row)
                receipt["reason"] = receipt["reason"] or "orphan_descendants"
                if row["ppid"] == os.getpid():
                    if _signal_owned(row, signal.SIGKILL):
                        receipt["signals"].append(dict(pid=row["pid"], start_ticks=row["start_ticks"], signal=signal.SIGKILL))
                    try:
                        pid, status, usage = os.wait4(row["pid"], os.WNOHANG)
                    except ChildProcessError:
                        continue
                    if pid:
                        code = os.waitstatus_to_exitcode(status)
                        receipt["reaped"].append(dict(pid=pid, start_ticks=row["start_ticks"],
                            exit_code=code, user_seconds=usage.ru_utime, system_seconds=usage.ru_stime))
                        if (process and pid == process.pid and receipt["native"]
                                and row["start_ticks"] == receipt["native"]["start_ticks"]):
                            process.returncode = code
                            receipt["native_exit_code"] = code
            time.sleep(0.01)
        for key in list(streams.get_map().values()):
            receipt["outputs"][key.data[0]]["truncated"] = True
            key.fileobj.close()
        streams.close()
        for handle in files:
            handle.close()
        receipt["processes"] = list(seen.values())
        receipt["process_coverage"] = "polled ancestry and adopted-child reaping; parent-reaped short-lived children may be absent"
        receipt["drain"] = dict(passed=not remaining and not scan_errors, remaining=remaining,
                                scan_errors=list(dict.fromkeys(scan_errors)))
        receipt["cancelled"] = any(item.startswith("signal:") for item in requested)
        receipt["timed_out"] = receipt["reason"] == "timeout"
        receipt["finished_monotonic_ns"] = time.monotonic_ns()
        receipt["elapsed_seconds"] = (receipt["finished_monotonic_ns"] - started) / 1e9
        receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
        receipt["cpu"] = {name: sum(row[name] for row in receipt["reaped"])
                          for name in ("user_seconds", "system_seconds")}
        receipt["passed"] = (receipt["native_exit_code"] == 0 and not receipt["reason"]
                             and not receipt["error"] and not receipt["cancelled"] and receipt["drain"]["passed"])
        try:
            connection.send(dict(kind="result", value=receipt))
        except (BrokenPipeError, EOFError, ConnectionResetError):
            # A dead controller cannot receive a receipt; cleanup still ran.
            pass
        finally:
            connection.close()


def run_owned(argv: list[str], cwd: Path, env: dict, outdir: Path, timeout_seconds: float,
              poll_seconds=1, observer=None, *, log_limit_bytes=32 * 1024 * 1024) -> dict:
    """Run one Linux workload synchronously; return only after logs close and cleanup.

    The observer runs in the caller, receives progress dictionaries, and may return
    JSON telemetry. It must not block. Invalid arguments raise before spawning.
    """
    if (sys.platform != "linux" or not hasattr(signal, "pidfd_send_signal")
            or not hasattr(os, "pidfd_open")):
        raise RuntimeError("Linux pidfds are required")
    if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
        raise RuntimeError("run_owned requires the default SIGCHLD disposition")
    if threading.current_thread() is not threading.main_thread() or threading.active_count() != 1:
        raise RuntimeError("run_owned requires a single-threaded main-thread controller")
    if (not isinstance(argv, list) or not argv or not argv[0] or any(not isinstance(x, str) or "\0" in x for x in argv)
            or not isinstance(env, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                or not k or "=" in k or "\0" in k + v for k, v in env.items())
            or not isinstance(log_limit_bytes, int) or log_limit_bytes <= 0
            or any(not isinstance(x, (int, float)) or isinstance(x, bool) or not math.isfinite(x)
                   or x <= 0 for x in (timeout_seconds, poll_seconds))
            or not Path(cwd).is_dir() or (observer is not None and not callable(observer))):
        raise ValueError("invalid owned-process arguments")
    outdir = Path(outdir).absolute()
    outdir.mkdir(mode=0o700, parents=False, exist_ok=False)
    parent, child = multiprocessing.get_context("fork").Pipe()
    controller = _identity(os.getpid())
    if controller is None:
        parent.close()
        child.close()
        raise RuntimeError("cannot establish controller identity")
    supervisor = multiprocessing.get_context("fork").Process(target=_supervise,
        args=(child, parent, argv, Path(cwd).absolute(), env, outdir, timeout_seconds,
              poll_seconds, log_limit_bytes, controller))
    pending, observed_signals, telemetry, result = [], [], [], None
    parent_error = None
    caller_deadline = time.monotonic() + timeout_seconds + 12
    previous = {number: signal.getsignal(number) for number in (signal.SIGINT, signal.SIGTERM)}
    for number in previous:
        def cancelled(signum, frame):
            observed_signals.append(signum)
            pending.append(f"signal:{signum}")
        signal.signal(number, cancelled)
    try:
        supervisor.start()
        child.close()
        while supervisor.is_alive() or parent.poll():
            if time.monotonic() > caller_deadline:
                raise TimeoutError("owned supervisor exceeded its cleanup bound")
            if pending:
                parent.send(pending.pop(0))
            if parent.poll(0.05):
                try:
                    message = parent.recv()
                except EOFError:
                    break
                if message["kind"] == "result":
                    result = message["value"]
                    break
                if observer is not None:
                    try:
                        value = observer(message["value"])
                        encoded = json.dumps(value, allow_nan=False)
                        if len(encoded) > 65536:
                            raise ValueError("observer sample exceeds 64 KiB")
                        if len(telemetry) >= 4096:
                            raise ValueError("observer sample limit")
                        telemetry.append(value)
                    except BaseException as exc:
                        parent.send(f"observer_error:{type(exc).__name__}: {exc}")
                        observer = None
        supervisor.join(7)
    except BaseException as exc:
        parent_error = exc
        if supervisor.pid is not None:
            identity = _identity(supervisor.pid)
            if identity:
                _signal_owned(identity, signal.SIGTERM)
            supervisor.join(7)
    finally:
        try:
            # Never abandon the supervisor when the caller transport/observer fails.
            if supervisor.pid is not None and supervisor.is_alive():
                identity = _identity(supervisor.pid)
                if identity:
                    _signal_owned(identity, signal.SIGTERM)
                deadline = time.monotonic() + 6
                while supervisor.is_alive() and time.monotonic() < deadline:
                    for row in _descendants(supervisor.pid):
                        if row["ppid"] == supervisor.pid:
                            _signal_owned(row, signal.SIGKILL)
                    supervisor.join(0.05)
                if supervisor.is_alive():
                    raise RuntimeError("owned supervisor did not drain; controller must remain alive")
        finally:
            parent.close()
            child.close()
            for number, handler in previous.items():
                signal.signal(number, handler)
    if parent_error is not None:
        raise parent_error
    if result is None:
        raise RuntimeError(f"owned supervisor failed without receipt (exit {supervisor.exitcode})")
    result["observer"] = telemetry
    if observed_signals:
        result["cancelled"] = True
        result["reason"] = result["reason"] or f"signal:{observed_signals[0]}"
        result["passed"] = False
    result["supervisor_exit_code"] = supervisor.exitcode
    result["passed"] = result["passed"] and supervisor.exitcode == 0
    result["logs_closed"] = True
    return result
