#!/usr/bin/env python3
"""Explicit Linux system-suspend controller; default operation only prints a plan.

No RTC alarm, power setting or inhibitor is modified. The execute flag requires
prior user coordination and manual wake after at least five seconds. CPU
self-tests inject a fake suspend operation and never invoke systemctl.
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

from frame_generation_process_pause_test import analyze_native_log

TAG = '[smoke_frame_generation_process_pause]'
SYSTEM_TAG = '[smoke_frame_generation_system_suspend]'
SUSPEND_COMMAND = ['/usr/bin/systemctl', 'suspend']
BILLION = 1_000_000_000


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while block := stream.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def file_identity(path):
    path = Path(path).resolve()
    require(path.is_file(), 'Explicit --data or HEAVEN_DATA must name the actual data file')
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest(path)}


def source_manifest(root):
    files = subprocess.check_output(['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others',
                                     '--exclude-standard']).decode().split('\0')
    manifest = {}
    for name in sorted(set(files) - {''}):
        path = root / name
        if not path.exists() and not path.is_symlink():
            continue
        manifest[name] = {'symlink': str(path.readlink())} if path.is_symlink() else digest(path)
    return {'git_head': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
            'files': manifest,
            'scope': 'Current tracked/nonignored checkout at run bounds; compiled-binary provenance is separate.'}


def audit_inputs_after(result, directory, root, source_before, data_before, read_manifest=source_manifest):
    """Retain a failed receipt even when post-run provenance cannot be read."""
    directory.mkdir(parents=True, exist_ok=True)
    try:
        source_after = read_manifest(root)
        after_path = directory / 'source-manifest-after.json'
        after_path.write_text(json.dumps(source_after, indent=2, sort_keys=True) + '\n')
        unchanged = source_before == source_after
        result['source_manifest'] = {'after_sha256': digest(after_path), 'unchanged': unchanged,
                                     'scope': source_before.get('scope', 'Run-bound source identity only')}
        if not unchanged:
            result['passed'] = False
            result['errors'].append('Checkout sources changed during execution')
    except (OSError, RuntimeError, UnicodeError, subprocess.SubprocessError) as error:
        result['passed'] = False
        result['errors'].append('Post-run source verification unavailable: ' + str(error))
        result['source_manifest'] = {'after_sha256': None, 'unchanged': False, 'unavailable_reason': str(error)}
    try:
        data_after = file_identity(data_before['path'])
        unchanged = data_before == data_after
        result['data_identity'] = {'before': data_before, 'after': data_after, 'unchanged': unchanged}
        if not unchanged:
            result['passed'] = False
            result['errors'].append('Actual data-file content changed during execution')
    except (OSError, RuntimeError) as error:
        result['passed'] = False
        result['errors'].append('Post-run data verification unavailable: ' + str(error))
        result['data_identity'] = {'before': data_before, 'after': None, 'unchanged': False,
                                   'unavailable_reason': str(error)}


def clocks():
    return {'boottime_ns': time.clock_gettime_ns(time.CLOCK_BOOTTIME),
            'monotonic_ns': time.clock_gettime_ns(time.CLOCK_MONOTONIC),
            'realtime_ns': time.time_ns()}


def clock_proof(before, after):
    boot = after['boottime_ns'] - before['boottime_ns']
    awake = after['monotonic_ns'] - before['monotonic_ns']
    require(boot >= 0 and awake >= 0, 'Independent suspend clocks rolled back')
    slept = max(0, boot - awake)
    require(slept >= 5 * BILLION, 'No independent proof of five seconds of physical system sleep')
    return {'before': before, 'after': after, 'awake_ns': awake, 'slept_ns': slept,
            'awake_under_250ms': awake < 250_000_000,
            'scope': 'Physical sleep proof. A small gate awake interval alone does not exclude all other reset causes.'}


def suspend_once(timeout, events):
    """The only real suspend call. Used solely behind the explicit CLI flag."""
    before = clocks()
    record = {'command': SUSPEND_COMMAND, 'before': before, 'exit_code': None}
    events.append(record)
    process = subprocess.Popen(SUSPEND_COMMAND, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = before['boottime_ns'] + int(timeout * BILLION)
    try:
        while True:
            after = clocks()
            if after['boottime_ns'] - before['boottime_ns'] - (
                    after['monotonic_ns'] - before['monotonic_ns']) >= 5 * BILLION:
                break
            status = process.poll()
            require(status in (None, 0), 'systemctl suspend failed')
            require(after['boottime_ns'] < deadline, 'System sleep/wake deadline exceeded')
            time.sleep(0.02)
        output, _ = process.communicate(timeout=3)
        record.update(exit_code=process.returncode, output=output, after=after)
        require(process.returncode == 0, 'systemctl suspend did not complete successfully')
        return clock_proof(before, after)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=3)
        if 'output' not in record:
            output, _ = process.communicate(timeout=3)
            record['output'] = output
        record['exit_code'] = process.returncode
        record['process_reaped'] = process.poll() is not None


def analyze(text, observations, gates, exit_code):
    # Reuse the maintained numeric source/count/owner oracle. Only the labelled
    # physical phases are normalized; independent native/controller clocks are
    # required below. This does not turn physical sleep into a SIGSTOP claim.
    normalized = text
    for physical, process in (
        ('system suspend asynchronous ring warmup', 'process pause asynchronous ring warmup'),
        ('queued source after system resume', 'queued source after process SIGCONT'),
        ('queued system suspend recovery', 'queued process pause recovery'),
        ('system suspend asynchronous ring recovery', 'process pause asynchronous ring recovery'),
        (SYSTEM_TAG + ' PASS: physical sleep, same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts',
         TAG + ' PASS: same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts'),
    ):
        normalized = normalized.replace(physical, process)
    errors, first, phases = analyze_native_log(normalized, observations, gates, exit_code)
    proofs = []
    for line in text.splitlines():
        if SYSTEM_TAG + ' clock-proof ' in line:
            proofs.append({key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)})
    paused = [proof for proof in proofs if observations and proof.get('source') == observations[0]['source']]
    if len(paused) != 1:
        errors.append('Missing or duplicate same-source native sleep-clock proof')
        return errors, first, phases, None
    proof = paused[0]
    try:
        require(proof['valid'] == 1, 'Native sleep clocks were unavailable')
        native = clock_proof(
            {'boottime_ns': proof['boot_before_ns'], 'monotonic_ns': proof['mono_before_ns']},
            {'boottime_ns': proof['boot_after_ns'], 'monotonic_ns': proof['mono_after_ns']})
        require(native['awake_ns'] == proof['awake_ns'] and native['slept_ns'] == proof['slept_ns'],
                'Native derived sleep/awake values disagree with raw clocks')
        require(proof['awake_under_250ms'] == int(native['awake_under_250ms']),
                'Native awake-threshold label disagrees with raw clocks')
        require(proof['serial'] == observations[0]['serial'], 'Native clock proof has another gate identity')
    except (RuntimeError, KeyError) as error:
        errors.append(str(error))
    return errors, first, phases, proof


def run_controlled(command, environment, directory, timeout, suspend_operation=suspend_once, native=True):
    directory.mkdir(parents=True, exist_ok=True)
    log_path = directory / 'native.log'
    require(not log_path.exists(), 'Preserve previous evidence; choose a fresh work directory')
    read_fd, write_fd = os.pipe()
    environment = dict(environment, HEAVEN_SMOKE_FG_PROCESS_PAUSE_FD=str(read_fd))
    process = reader = None
    lines = queue.Queue()
    gates, observations, sleep_events, errors = {}, [], [], []
    started = clocks()
    before_hash = digest(__file__)
    exit_code = None
    with log_path.open('w', encoding='utf-8') as log:
        try:
            process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, bufsize=1, pass_fds=(read_fd,), start_new_session=True)
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
            while True:
                require(clocks()['boottime_ns'] - started['boottime_ns'] < timeout * BILLION,
                        'System-suspend controller deadline exceeded')
                try:
                    line = lines.get(timeout=0.1)
                except queue.Empty:
                    continue
                if line is None:
                    break
                if TAG not in line or not any(event in line for event in ('gate-enter', 'packet-queued')):
                    continue
                fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
                serial = int(fields['serial'])
                gate = gates.setdefault(serial, {})
                key = 'entered' if 'gate-enter' in line else 'queued'
                require(key not in gate, 'Duplicate gate protocol event')
                gate[key] = fields
                if 'entered' not in gate or 'queued' not in gate:
                    continue
                entered, queued = gate['entered'], gate['queued']
                require(entered['source'] == queued['source'] and entered['mode'] == queued['mode'],
                        'Gate and accepted packet identities differ')
                require(queued['accepted'] in ('0', '1'), 'Invalid accepted source marker')
                if queued['accepted'] == '0':
                    release = b'R'
                elif queued['mode'] == 'control':
                    require(queued['ready'] == '1', 'Control packet has no valid history')
                    release = b'C'
                else:
                    require(queued['mode'] == 'pause' and queued['ready'] == '1', 'Paused packet has no valid history')
                    require(any(item.get('release') == 'C' for item in gates.values()), 'Missing preceding no-suspend control')
                    require(not observations and not sleep_events, 'Only one system suspend is permitted')
                    proof = suspend_operation(min(timeout, 90), sleep_events)
                    require(process.poll() is None, 'Smoke process exited during system suspend')
                    observations.append({'serial': serial, 'source': int(queued['source']),
                                         'queued_epoch': int(queued['epoch']), 'controller_clock_proof': proof})
                    release = b'P'
                os.write(write_fd, release)
                gate['release'] = release.decode()
            exit_code = process.wait(timeout=5)
        except (Exception, KeyboardInterrupt) as error:
            errors.append(str(error))
        finally:
            if read_fd >= 0:
                os.close(read_fd)
            os.close(write_fd)  # EOF always releases a blocked native callback.
            if process is not None:
                if process.poll() is None:
                    try:
                        process.wait(timeout=12)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=3)
                exit_code = process.returncode
            if reader is not None:
                reader.join(timeout=3)
    text = log_path.read_text()
    first = native_proof = None
    phases = {}
    if native:
        findings, first, phases, native_proof = analyze(text, observations, gates, exit_code)
        errors.extend(findings)
    if 'Validation Error' in text or 'Validation Warning' in text:
        errors.append('Vulkan validation reported an error or warning')
    after_hash = digest(__file__)
    if before_hash != after_hash:
        errors.append('Controller bytes changed during execution')
    result = {'schema_version': 1, 'passed': not errors, 'native_gpu_run': native, 'command': command,
              'exit_code': exit_code, 'errors': errors, 'process_reaped': process is None or process.poll() is not None,
              'started_clocks': started, 'ended_clocks': clocks(), 'gates': gates, 'system_suspends': sleep_events,
              'physical_observations': observations, 'first_resumed_source': first, 'native_phases': phases,
              'native_clock_proof': native_proof, 'log_sha256': digest(log_path), 'controller_sha256': after_hash,
              'validation_errors': text.count('Validation Error'), 'validation_warnings': text.count('Validation Warning'),
              'clock_specific_qualification': 'Awake-gate threshold is reported separately; other reset causes/previous native interval are not excluded.',
              'scope': 'Opt-in physical sleep, exact queued-source reset/recovery and ownership. No display cadence, physical latency or other-platform acceptance. Timeouts/cleanup run only while the host is awake.'}
    (directory / 'receipt.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def self_test(directory):
    child = r'''
import os,time
fd=int(os.environ['HEAVEN_SMOKE_FG_PROCESS_PAUSE_FD'])
tag='[smoke_frame_generation_process_pause]'
for serial,mode,accepted,source in [(1,'pause',0,16),(2,'control',1,17),(3,'pause',1,18)]:
 messages=[f'{tag} gate-enter serial={serial} source={source} mode={mode}',f'{tag} packet-queued serial={serial} source={source} mode={mode} accepted={accepted} epoch=7 ready=1']
 if serial%2: messages.reverse()
 for message in messages: print(message,flush=True)
 expected=b'R' if not accepted else b'P' if mode=='pause' else b'C'
 assert os.read(fd,1)==expected
'''
    calls = []

    def fake_suspend(timeout, events):
        calls.append(timeout)
        events.append({'simulated': True, 'command': 'none; CPU self-test'})
        return clock_proof({'boottime_ns': BILLION, 'monotonic_ns': BILLION},
                           {'boottime_ns': 7 * BILLION + 100_000_000, 'monotonic_ns': BILLION + 100_000_000})

    positive = run_controlled([sys.executable, '-u', '-c', child], os.environ, directory / 'protocol-positive',
                              10, fake_suspend, native=False)
    require(positive['passed'] and positive['exit_code'] == 0 and len(calls) == 1,
            'CPU protocol did not perform exactly one fake suspend after accepted G+Q')
    require(positive['gates'][1]['release'] == 'R' and positive['gates'][2]['release'] == 'C',
            'Rejected/control packets reached fake suspend')
    for before, after in (
        ({'boottime_ns': BILLION, 'monotonic_ns': BILLION}, {'boottime_ns': 7*BILLION, 'monotonic_ns': 7*BILLION}),
        ({'boottime_ns': BILLION, 'monotonic_ns': BILLION}, {'boottime_ns': 0, 'monotonic_ns': BILLION}),
    ):
        try:
            clock_proof(before, after)
        except RuntimeError:
            pass
        else:
            raise RuntimeError('No-sleep/rollback clock negative passed')

    def failed_suspend(timeout, events):
        events.append({'simulated': True, 'failure': 'test command failure'})
        raise RuntimeError('test command failure')

    failure = run_controlled([sys.executable, '-u', '-c', child], os.environ, directory / 'command-failure',
                             10, failed_suspend, native=False)
    require(not failure['passed'] and failure['process_reaped'] and 'test command failure' in failure['errors'],
            'Command failure did not release/reap the child')
    timeout = run_controlled([sys.executable, '-u', '-c', 'import time;time.sleep(30)'], os.environ,
                             directory / 'timeout', 0.1, fake_suspend, native=False)
    require(not timeout['passed'] and timeout['process_reaped'] and
            'System-suspend controller deadline exceeded' in timeout['errors'], 'Timeout did not reap child')
    asset = directory / 'data-hash-control.bin'
    asset.write_bytes(b'asset original')
    original_asset = file_identity(asset)
    require(file_identity(asset) == original_asset, 'Stable data identity did not repeat')
    asset.write_bytes(b'asset modified')
    require(file_identity(asset)['sha256'] != original_asset['sha256'] and
            file_identity(asset)['bytes'] == original_asset['bytes'], 'Same-size data tampering was not detected')
    source_before = {'files': {'fixture.cpp': 'unchanged'}, 'scope': 'CPU provenance control'}
    read_stable = lambda root: source_before
    tampered = {'passed': True, 'errors': []}
    audit_inputs_after(tampered, directory / 'tampered-asset', directory, source_before, original_asset, read_stable)
    require(not tampered['passed'] and not tampered['data_identity']['unchanged'], 'Tampered asset retained a passed receipt')
    asset.unlink()
    missing = {'passed': True, 'errors': []}
    audit_inputs_after(missing, directory / 'missing-asset', directory, source_before, original_asset, read_stable)
    require(not missing['passed'] and missing['data_identity']['after'] is None, 'Missing asset retained a passed receipt')
    asset.write_bytes(b'asset original')
    changed = {'passed': True, 'errors': []}
    audit_inputs_after(changed, directory / 'changed-source', directory, source_before, original_asset,
                       lambda root: {'files': {'fixture.cpp': 'modified'}})
    require(not changed['passed'] and not changed['source_manifest']['unchanged'], 'Changed source retained a passed receipt')

    def git_failure(root):
        raise subprocess.CalledProcessError(128, ['git', 'test-only'])

    failed_git = {'passed': True, 'errors': []}
    audit_inputs_after(failed_git, directory / 'git-failure', directory, source_before, original_asset, git_failure)
    require(not failed_git['passed'] and failed_git['source_manifest']['after_sha256'] is None,
            'Git failure retained a passed receipt')
    sample = '''
[smoke_frame_generation] system suspend asynchronous ring warmup: +128 real, +128 generated, +128 native real, +128 native generated, epoch 7
[smoke_frame_generation] queued no-pause control: +1 real, +1 generated, +1 native real, +1 native generated, epoch 7
[smoke_frame_generation] queued source after system resume: +1 real, +0 generated, +1 native real, +0 native generated, epoch 8
[smoke_frame_generation_process_pause] first-resumed source=18 epoch_before=7 epoch_after=8 generated_before=134 generated_after=134 native_generated_before=134 native_generated_after=134
[smoke_frame_generation_system_suspend] clock-proof serial=3 source=18 valid=1 boot_before_ns=1000000000 boot_after_ns=7100000000 mono_before_ns=1000000000 mono_after_ns=1100000000 awake_ns=100000000 slept_ns=6000000000 awake_under_250ms=1
[smoke_frame_generation] queued system suspend recovery: +5 real, +4 generated, +5 native real, +4 native generated, epoch 8
[smoke_frame_generation] system suspend asynchronous ring recovery: +128 real, +128 generated, +128 native real, +128 native generated, epoch 8
[smoke_frame_generation_system_suspend] PASS: physical sleep, same queued source reset, five-source recovery, 256 asynchronous sources and exact owner counts
[smoke] case 'frame_generation': PASS
'''
    observations = [{'source': 18, 'queued_epoch': 7, 'serial': 3}]
    gates = {2: {'release': 'C', 'queued': {'source': '17', 'epoch': '7'}}}
    require(not analyze(sample, observations, gates, 0)[0], 'Native numeric/clock proof positive failed')
    negatives = [
        (sample.replace('epoch_after=8', 'epoch_after=7'), 'stale wall-time history'),
        (sample.replace('generated_after=134 native_', 'generated_after=135 native_'), 'recorded generated work'),
        (sample.replace('mono_after_ns=1100000000', 'mono_after_ns=7100000000'), 'physical system sleep'),
        (sample.replace('slept_ns=6000000000', 'slept_ns=6000000001'), 'derived sleep/awake'),
        (sample.replace('serial=3 source=18 valid=1', 'serial=4 source=18 valid=1'), 'another gate identity'),
        (sample.replace('serial=3 source=18 valid=1', 'serial=3 source=19 valid=1'), 'same-source native sleep'),
        (sample.replace('valid=1 boot_', 'valid=0 boot_'), 'clocks were unavailable'),
        (sample.replace('awake_under_250ms=1', 'awake_under_250ms=0'), 'awake-threshold label'),
    ]
    for text, expected in negatives:
        require(any(expected in error for error in analyze(text, observations, gates, 0)[0]),
                'Native proof negative passed: ' + expected)
    return {'passed': True, 'native_gpu_run': False, 'real_suspend_calls': 0,
            'checks': ['reversed G/Q events', 'rejected/control release', 'exactly one fake suspend',
                       'independent boottime-minus-monotonic proof', 'no-sleep and rollback rejection',
                       'command failure releases/reaps child', 'deadline terminates/reaps child',
                       'streaming data hash repeats and rejects same-size content tampering',
                       'missing/tampered data and changed/unavailable source retain failed receipts',
                       'native source/count/clock positive and eight numeric/identity negatives']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--data', type=Path)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--execute-system-suspend', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    require(sys.platform == 'linux' and hasattr(time, 'CLOCK_BOOTTIME'), 'Linux BOOTTIME support is required')
    require(30 <= args.timeout <= 300, 'Timeout must be between 30 and 300 seconds')
    require(not (args.self_test and args.execute_system_suspend), 'Self-test cannot execute system suspend')
    if args.self_test:
        directory = args.work_dir / f'self-test-{os.getpid()}-{time.monotonic_ns()}'
        result = self_test(directory)
        result['work_dir'] = str(directory)
    elif not args.execute_system_suspend:
        result = {'plan_only': True, 'real_suspend_calls': 0, 'app_started': False,
                  'suspend_command': SUSPEND_COMMAND,
                  'requirements': ['Explicit user authorization and manual wake after at least five seconds',
                                   'Docker build/performance runs stopped', 'Real FSR smoke binary and native display',
                                   'Explicit --data or HEAVEN_DATA file hashed before and after execution',
                                   'No RTC alarm is armed; no power setting or inhibitor is changed'],
                  'execute_flag': '--execute-system-suspend'}
    else:
        require(args.binary and args.binary.is_file(), 'Real smoke --binary is required')
        data = args.data or os.environ.get('HEAVEN_DATA')
        require(data, 'Physical execution requires explicit --data or HEAVEN_DATA')
        data_before = file_identity(data)
        root = Path(__file__).resolve().parents[4]
        source_before = source_manifest(root)
        args.work_dir.mkdir(parents=True, exist_ok=True)
        before_manifest = args.work_dir / 'source-manifest-before.json'
        require(not before_manifest.exists(), 'Preserve previous evidence; choose a fresh work directory')
        before_manifest.write_text(json.dumps(source_before, indent=2, sort_keys=True) + '\n')
        before_manifest_sha256 = digest(before_manifest)
        (args.work_dir / 'data-before.json').write_text(json.dumps(data_before, indent=2) + '\n')
        environment = dict(os.environ, HEAVEN_SMOKE='1', HEAVEN_SMOKE_CASE='frame_generation',
                           HEAVEN_SMOKE_FRAME_GENERATION='1', HEAVEN_SMOKE_FG_SYSTEM_SUSPEND='1',
                           HEAVEN_SMOKE_EXPECT_API='Vulkan',
                           HEAVEN_USER_DIR=str(args.work_dir.resolve() / 'user'))
        for key in ('HEAVEN_SMOKE_FG_PROCESS_PAUSE', 'HEAVEN_SMOKE_FG_UI_ORACLE', 'HEAVEN_SMOKE_FG_BROKEN_CHAIN_ORACLE',
                    'HEAVEN_FG_CAPTURE_DIR', 'HEAVEN_FG_CAPTURE_UI_INPUTS', 'HEAVEN_FG_CAPTURE_START_SOURCE'):
            environment.pop(key, None)
        environment['HEAVEN_DATA'] = data_before['path']
        binary_before = digest(args.binary)
        result = run_controlled([str(args.binary.resolve())], environment, args.work_dir, args.timeout)
        result['binary_sha256_before'] = binary_before
        try:
            result['binary_sha256'] = digest(args.binary)
        except OSError as error:
            result['binary_sha256'] = None
            result['passed'] = False
            result['errors'].append('Post-run binary verification unavailable: ' + str(error))
        audit_inputs_after(result, args.work_dir, root, source_before, data_before)
        result['source_manifest']['before_sha256'] = before_manifest_sha256
        result['environment'] = {name: environment[name] for name in (
            'DISPLAY', 'WAYLAND_DISPLAY', 'SDL_VIDEODRIVER', 'VK_INSTANCE_LAYERS', 'VK_LAYER_PATH',
            'HEAVEN_USER_DIR', 'HEAVEN_DATA') if name in environment}
        since = str(result['started_clocks']['realtime_ns'] // BILLION)
        journal_command = ['/usr/bin/journalctl', '-b', '-u', 'systemd-suspend.service',
                           '--since', '@' + since, '--no-pager', '-o', 'short-iso']
        try:
            journal = subprocess.run(journal_command, capture_output=True, text=True, timeout=10)
            journal_path = args.work_dir / 'systemd-suspend.log'
            journal_path.write_text(journal.stdout + journal.stderr)
            result['systemd_journal'] = {'command': journal_command, 'exit_code': journal.returncode,
                                         'sha256': digest(journal_path), 'scope': 'Supplementary service log; independent clocks are the sleep proof.'}
        except (OSError, subprocess.TimeoutExpired) as error:
            result['systemd_journal'] = {'command': journal_command, 'unavailable_reason': str(error)}
        if result['binary_sha256'] is not None and binary_before != result['binary_sha256']:
            result['passed'] = False
            result['errors'].append('Smoke executable changed during execution')
        (args.work_dir / 'receipt.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result.get('passed', result.get('plan_only', False)) else 1


if __name__ == '__main__':
    raise SystemExit(main())
