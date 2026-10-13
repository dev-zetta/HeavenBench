#!/usr/bin/env python3
"""Opt-in native Linux queued-callback SIGSTOP/SIGCONT regression controller.

Launch the smoke binary directly, with a usable native display and optional
Vulkan validation environment. This is not an ordinary GPU-free CTest entry.
--self-test exercises only the controller with a small CPU child process.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import sys
import threading
import time


TAG = "[smoke_frame_generation_process_pause]"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def task_states(pid):
    tasks = Path(f"/proc/{pid}/task")
    result = {}
    for task in sorted(tasks.iterdir(), key=lambda path: int(path.name)):
        fields = dict(line.split(":", 1) for line in (task / "status").read_text().splitlines()
                      if ":" in line)
        result[int(task.name)] = fields["State"].strip().split()[0]
    return result


def wait_all_stopped(pid):
    deadline = time.monotonic() + 3
    previous = None
    while time.monotonic() < deadline:
        try:
            current = task_states(pid)
        except (FileNotFoundError, ProcessLookupError):
            current = {}
        if current and all(state in ("T", "t") for state in current.values()):
            if current == previous:
                return current
            previous = current
        else:
            previous = None
        time.sleep(0.02)
    raise RuntimeError("SIGSTOP did not produce stable all-thread stopped membership")


def analyze_native_log(text, stops, gates, exit_code):
    """Check actual numeric observations independently of the C++ PASS marker."""
    errors = []
    first = re.search(r"first-resumed source=(\d+) epoch_before=(\d+) epoch_after=(\d+) "
                      r"generated_before=(\d+) generated_after=(\d+) "
                      r"native_generated_before=(\d+) native_generated_after=(\d+)", text)
    first_source = None
    if first:
        first_source = dict(zip(("source", "epoch_before", "epoch_after", "generated_before",
                                 "generated_after", "native_generated_before", "native_generated_after"),
                                map(int, first.groups())))
    phase_pattern = (r"\[smoke_frame_generation\] ([^:\n]+): \+(\d+) real, \+(\d+) generated, "
                     r"\+(\d+) native real, \+(\d+) native generated, epoch (\d+)")
    required_phases = ("queued no-pause control", "queued source after process SIGCONT",
                       "process pause asynchronous ring warmup", "process pause asynchronous ring recovery",
                       "queued process pause recovery")
    phases = {}
    for match in re.finditer(phase_pattern, text):
        name = match.group(1)
        # The ordinary lifecycle deliberately repeats resize labels. Only the
        # independently checked process-pause phases have unique identities.
        if name not in required_phases:
            continue
        if name in phases:
            errors.append(f"Duplicate native phase observation: {name}")
            continue
        phases[name] = dict(zip(("real", "generated", "native_real", "native_generated", "epoch"),
                                map(int, match.groups()[1:])))
    if exit_code != 0:
        errors.append(f"Native smoke exited {exit_code}")
    if len(stops) != 1 or first_source is None:
        errors.append("Missing whole-process stop proof or same-source result")
    else:
        stop = stops[0]
        if first_source["source"] != stop["source"]:
            errors.append("First resumed result has another source identity")
        if first_source["epoch_before"] != stop["queued_epoch"]:
            errors.append("First resumed epoch differs from the pre-stop queued packet")
        if first_source["epoch_after"] <= first_source["epoch_before"]:
            errors.append("Queued-source callback reused stale wall-time history")
        if first_source["generated_after"] != first_source["generated_before"]:
            errors.append("First resumed source recorded generated work")
        if first_source["native_generated_after"] != first_source["native_generated_before"]:
            errors.append("First resumed source submitted a generated image")
        controls = [gate["queued"] for gate in gates.values() if gate.get("release") == "C"]
        if len(controls) != 1 or int(controls[0]["source"]) + 1 != stop["source"] or \
                int(controls[0]["epoch"]) != stop["queued_epoch"]:
            errors.append("No-pause control is not the immediately preceding source/history")
    for name in ("queued no-pause control", "queued source after process SIGCONT"):
        phase = phases.get(name)
        expected_generated = 1 if name == "queued no-pause control" else 0
        if phase is None or phase["real"] != 1 or phase["native_real"] != 1 or \
                phase["generated"] != expected_generated or phase["native_generated"] != expected_generated:
            errors.append(f"Incorrect single-source native accounting: {name}")
    for name in ("process pause asynchronous ring warmup", "process pause asynchronous ring recovery"):
        phase = phases.get(name)
        if phase is None or phase["real"] != 128 or phase["native_real"] != 128 or \
                not 0 < phase["generated"] <= 128 or phase["native_generated"] != phase["generated"]:
            errors.append(f"Incorrect 128-source asynchronous native accounting: {name}")
    recovery = phases.get("queued process pause recovery")
    if recovery is None or recovery["real"] != 5 or recovery["native_real"] != 5 or \
            not 0 < recovery["generated"] <= 5 or recovery["native_generated"] != recovery["generated"]:
        errors.append("Generation did not recover within five exactly accounted sources")
    if "PASS: same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts" not in text:
        errors.append("Missing asynchronous queued-source/reset/recovery/owner oracle PASS")
    if "case 'frame_generation': PASS" not in text:
        errors.append("Lifecycle did not complete and restore its owners")
    return errors, first_source, phases


def run_controlled(command, environment, directory, hold_seconds, timeout, native=True):
    directory.mkdir(parents=True, exist_ok=True)
    log_path = directory / "native.log"
    require(not log_path.exists(), "Preserve existing process-pause evidence; use a fresh work directory")
    read_fd, write_fd = os.pipe()
    environment = dict(environment, HEAVEN_SMOKE_FG_PROCESS_PAUSE_FD=str(read_fd))
    process = None
    reader = None
    lines = queue.Queue()
    gates = {}
    stops = []
    errors = []
    start = time.monotonic()
    controller_before = digest(__file__)
    exit_code = None
    with log_path.open("w", encoding="utf-8") as log:
        try:
            process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1,
                                       pass_fds=(read_fd,), start_new_session=True)
            os.close(read_fd)
            read_fd = -1

            def read_output():
                try:
                    for line in process.stdout:
                        log.write(line)
                        log.flush()
                        lines.put(line)
                finally:
                    lines.put(None)

            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            ended = False
            while not ended:
                require(time.monotonic() - start < timeout, "Process-pause controller deadline exceeded")
                try:
                    line = lines.get(timeout=0.1)
                except queue.Empty:
                    continue
                if line is None:
                    ended = True
                    continue
                if TAG not in line or not any(event in line for event in ("gate-enter", "packet-queued")):
                    continue
                fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
                serial = int(fields["serial"])
                gate = gates.setdefault(serial, {})
                event = "entered" if "gate-enter" in line else "queued"
                require(event not in gate, "Duplicate gate protocol event")
                gate[event] = fields
                if "entered" not in gate or "queued" not in gate:
                    continue
                entered, queued = gate["entered"], gate["queued"]
                require(entered["source"] == queued["source"] and entered["mode"] == queued["mode"],
                        "Gate and queued packet source identities differ")
                require(queued["accepted"] in ("0", "1"), "Invalid accepted-source marker")
                if queued["accepted"] == "0":
                    release = b"R"
                elif queued["mode"] == "control":
                    require(queued["ready"] == "1", "No-pause control queued without history")
                    release = b"C"
                else:
                    require(queued["mode"] == "pause" and queued["ready"] == "1",
                            "Invalid paused-source mode/history")
                    require(any(item.get("release") == "C" for item in gates.values()),
                            "Paused source preceded the no-pause control")
                    require(not stops, "More than one accepted process-pause source")
                    # Both G and Q are observed before stopping: the real SDK
                    # packet already exists and no post-resume prepare can
                    # supply the frontend's ordinary long-gap reset instead.
                    sent_stop = time.monotonic_ns()
                    os.kill(process.pid, signal.SIGSTOP)
                    stopped = wait_all_stopped(process.pid)
                    hold_start = time.monotonic_ns()
                    time.sleep(hold_seconds)
                    final = task_states(process.pid)
                    require(final == stopped and all(state in ("T", "t") for state in final.values()),
                            "Stopped task membership changed during the hold")
                    held_ns = time.monotonic_ns() - hold_start
                    require(held_ns >= int(hold_seconds * 1e9), "Measured stopped interval is too short")
                    os.kill(process.pid, signal.SIGCONT)
                    stops.append({"serial": serial, "source": int(queued["source"]),
                                  "queued_epoch": int(queued["epoch"]), "stop_sent_cpu_monotonic_ns": sent_stop,
                                  "all_tasks_stopped_cpu_monotonic_ns": hold_start,
                                  "stopped_hold_ns": held_ns, "tasks_before_and_after_hold": stopped,
                                  "task_count": len(stopped), "scope": "Whole process CPU suspension, not system suspend or display feedback"})
                    release = b"P"
                os.write(write_fd, release)
                gate["release"] = release.decode()
            exit_code = process.wait(timeout=5)
        except (Exception, KeyboardInterrupt) as error:
            errors.append(str(error))
        finally:
            if process is not None and process.poll() is None:
                # Always wake a stopped child, including failures before our
                # stopped-state receipt. Closing the pipe releases any gate.
                try:
                    os.kill(process.pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            if read_fd >= 0:
                os.close(read_fd)
            os.close(write_fd)
            if process is not None:
                if process.poll() is None:
                    try:
                        process.wait(timeout=12)
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)
                exit_code = process.returncode
            if reader is not None:
                reader.join(timeout=3)
    text = log_path.read_text()
    first_source = None
    phases = {}
    if native:
        observations, first_source, phases = analyze_native_log(text, stops, gates, exit_code)
        errors.extend(observations)
    if "Validation Error" in text or "Validation Warning" in text:
        errors.append("Vulkan validation reported an error or warning")
    controller_after = digest(__file__)
    if controller_before != controller_after:
        errors.append("Controller bytes changed during execution")
    report = {"schema": 1, "passed": not errors, "command": command,
              "pid": process.pid if process else None, "exit_code": exit_code,
              "elapsed_seconds": time.monotonic() - start, "errors": errors,
              "native_gpu_run": native, "process_reaped": process is None or process.poll() is not None,
              "gates": gates, "process_stops": stops, "first_resumed_source": first_source, "native_phases": phases,
              "exact_queued_gap_oracle_failed": "queued callback interpolated across OS process suspension" in text,
              "validation_errors": text.count("Validation Error"), "validation_warnings": text.count("Validation Warning"),
              "log_sha256": digest(log_path), "controller_sha256": controller_after,
              "controller_sha256_before": controller_before, "controller_unchanged": controller_before == controller_after,
              "scope": "Queued native callback and asynchronous lifecycle; no pixel, cadence, latency, physical system-suspend or platform-wide acceptance"}
    (directory / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def self_test(directory):
    child = """
import os,sys,threading,time
fd=int(os.environ['HEAVEN_SMOKE_FG_PROCESS_PAUSE_FD'])
for _ in range(3):threading.Thread(target=lambda:time.sleep(30),daemon=True).start()
tag='[smoke_frame_generation_process_pause]'
for serial,mode,accepted in [(1,'pause',0),(2,'control',1),(3,'pause',1)]:
 messages=[f'{tag} gate-enter serial={serial} source=17 mode={mode}',f'{tag} packet-queued serial={serial} source=17 mode={mode} accepted={accepted} epoch=7 ready=1']
 if serial%2:messages.reverse()
 for message in messages:print(message,flush=True)
 expected=b'R' if not accepted else b'P' if mode=='pause' else b'C'
 assert os.read(fd,1)==expected
"""
    positive = run_controlled([sys.executable, "-u", "-c", child], os.environ,
                              directory / "protocol-positive", 0.5, 10, native=False)
    require(positive["passed"] and positive["exit_code"] == 0 and
            len(positive["process_stops"]) == 1 and positive["process_stops"][0]["task_count"] == 4,
            "Controller positive protocol/all-thread proof failed")
    blocked = "import os,signal,time;os.kill(os.getpid(),signal.SIGSTOP);time.sleep(30)"
    negative = run_controlled([sys.executable, "-u", "-c", blocked], os.environ,
                              directory / "timeout-cleanup", 0.5, 1, native=False)
    require(not negative["passed"] and negative["process_reaped"] and
            "Process-pause controller deadline exceeded" in negative["errors"],
            "Controller timeout did not resume/reap the stopped child")
    sample = """
[smoke_frame_generation] process pause asynchronous ring warmup: +128 real, +128 generated, +128 native real, +128 native generated, epoch 7
[smoke_frame_generation] queued no-pause control: +1 real, +1 generated, +1 native real, +1 native generated, epoch 7
[smoke_frame_generation] queued source after process SIGCONT: +1 real, +0 generated, +1 native real, +0 native generated, epoch 8
[smoke_frame_generation_process_pause] first-resumed source=18 epoch_before=7 epoch_after=8 generated_before=134 generated_after=134 native_generated_before=134 native_generated_after=134
[smoke_frame_generation] queued process pause recovery: +5 real, +4 generated, +5 native real, +4 native generated, epoch 8
[smoke_frame_generation] process pause asynchronous ring recovery: +128 real, +128 generated, +128 native real, +128 native generated, epoch 8
[smoke_frame_generation] resized first source: +1 real, +0 generated, +1 native real, +0 native generated, epoch 10
[smoke_frame_generation] resized recovery: +5 real, +5 generated, +5 native real, +5 native generated, epoch 10
[smoke_frame_generation] resized first source: +1 real, +0 generated, +1 native real, +0 native generated, epoch 12
[smoke_frame_generation] resized recovery: +5 real, +5 generated, +5 native real, +5 native generated, epoch 12
[smoke_frame_generation_process_pause] PASS: same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts
[smoke] case 'frame_generation': PASS
"""
    stops = [{"source": 18, "queued_epoch": 7}]
    gates = {1: {"release": "C", "queued": {"source": "17", "epoch": "7"}}}
    require(not analyze_native_log(sample, stops, gates, 0)[0], "Independent numeric receipt positive failed")
    numeric_negatives = [
        (sample.replace("epoch_after=8", "epoch_after=7"), stops, "stale wall-time history"),
        (sample.replace("generated_after=134 native_", "generated_after=135 native_"), stops, "recorded generated work"),
        (sample.replace("native_generated_after=134", "native_generated_after=135"), stops, "submitted a generated image"),
        (sample, [{"source": 18, "queued_epoch": 6}], "pre-stop queued packet"),
        (sample.replace("first-resumed source=18", "first-resumed source=19"), stops, "another source identity"),
        (sample.replace("+128 native real", "+127 native real", 1), stops, "128-source asynchronous"),
        (sample.replace("+1 generated, +1 native real", "+0 generated, +1 native real", 1), stops, "single-source native accounting"),
    ]
    for text, source_stops, expected in numeric_negatives:
        require(any(expected in message for message in analyze_native_log(text, source_stops, gates, 0)[0]),
                f"Numeric negative control was not rejected: {expected}")
    for line in sample.splitlines():
        if line.startswith("[smoke_frame_generation]") and "resized" not in line:
            require(any("Duplicate native phase observation" in message
                        for message in analyze_native_log(sample + line + "\n", stops, gates, 0)[0]),
                    f"Duplicate required phase was not rejected: {line}")
    return {"passed": True, "native_gpu_run": False,
            "checks": ["reversed G/Q ordering", "rejected-attempt release without stop", "no-pause release",
                       "stable four-thread SIGSTOP hold", "SIGCONT recovery", "timeout resumes and reaps stopped child",
                       "independent receipt positive with repeated ordinary resize labels",
                       "seven numeric negatives and five duplicate-required-phase negatives"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--hold-seconds", type=float, default=1)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    require(sys.platform == "linux", "Process-pause controller requires Linux /proc and signals")
    require(0.5 <= args.hold_seconds <= 3 and 10 <= args.timeout <= 300, "Invalid bounded controller duration")
    if args.self_test:
        result = self_test(args.work_dir)
    else:
        require(args.binary and args.binary.is_file(), "--binary must name the real smoke executable")
        environment = dict(os.environ, HEAVEN_SMOKE="1", HEAVEN_SMOKE_CASE="frame_generation",
                           HEAVEN_SMOKE_FRAME_GENERATION="1", HEAVEN_SMOKE_FG_PROCESS_PAUSE="1",
                           HEAVEN_SMOKE_EXPECT_API="Vulkan")
        for key in ("HEAVEN_SMOKE_FG_UI_ORACLE", "HEAVEN_FG_CAPTURE_DIR", "HEAVEN_FG_CAPTURE_UI_INPUTS",
                    "HEAVEN_FG_CAPTURE_START_SOURCE"):
            environment.pop(key, None)
        if args.data:
            environment["HEAVEN_DATA"] = str(args.data.resolve())
        binary_before = digest(args.binary)
        result = run_controlled([str(args.binary.resolve())], environment,
                                args.work_dir, args.hold_seconds, args.timeout)
        result["binary_sha256"] = digest(args.binary)
        result["binary_sha256_before"] = binary_before
        result["binary_unchanged"] = binary_before == result["binary_sha256"]
        if not result["binary_unchanged"]:
            result["passed"] = False
            result["errors"].append("Native executable bytes changed during execution")
        (args.work_dir / "receipt.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
