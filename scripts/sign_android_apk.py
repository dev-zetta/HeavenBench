#!/usr/bin/env python3
"""Re-sign an APK with one pinned certificate; keep signing credentials temporary."""
import argparse
import base64
import binascii
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
import zlib

MAX_KEYSTORE_BYTES = 1024 * 1024
MAX_APK_BYTES = 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 4 * MAX_APK_BYTES
PASSWORD_ENV = 'HEAVEN_ANDROID_KEYSTORE_PASSWORD'
KEYSTORE_ENV = 'HEAVEN_ANDROID_KEYSTORE_BASE64'
APPLICATION_ID = 'app.zetta.heavenbench'


class SigningError(Exception):
    """A public, non-secret failure category."""


def require(condition, message):
    if not condition:
        raise SigningError(message)


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def checked_path(path, *, existing=True, directory=False):
    path = Path(os.path.abspath(path))
    for part in (*reversed(path.parents), path):
        require(not part.is_symlink(), 'Symlink paths are not accepted.')
    if existing:
        require(path.is_dir() if directory else path.is_file(), 'Required input or directory is unavailable.')
    return path


def identity(path):
    info = path.stat()
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, file_digest(path))


def jar_signature_entry(name):
    # AOSP apksig's reserved, direct META-INF signature metadata only.
    if not name.startswith('META-INF/'):
        return False
    leaf = name[len('META-INF/'):].upper()
    return '/' not in leaf and (leaf == 'MANIFEST.MF' or leaf.startswith('SIG-') or
                                leaf.endswith(('.SF', '.RSA', '.DSA', '.EC')))


def zip_payload(path):
    result, seen, expanded = {}, set(), 0
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        require(0 < len(entries) <= 100000, 'APK ZIP entry count is invalid.')
        for entry in entries:
            name = entry.filename
            pure = PurePosixPath(name)
            mode = stat.S_IFMT(entry.external_attr >> 16)
            require(name and entry.orig_filename == name and '\\' not in name and ':' not in name and
                    not pure.is_absolute() and '..' not in pure.parts and
                    pure.as_posix() == name.rstrip('/') and
                    not any(ord(c) < 32 or ord(c) == 127 for c in name), 'APK ZIP contains an unsafe path.')
            require(name.rstrip('/') not in seen and mode in (0, stat.S_IFREG, stat.S_IFDIR) and
                    not (entry.flag_bits & 1), 'APK ZIP contains duplicate, special or encrypted entries.')
            seen.add(name.rstrip('/'))
            expanded += entry.file_size
            require(entry.file_size <= MAX_APK_BYTES and expanded <= MAX_EXPANDED_BYTES,
                    'APK ZIP expanded data exceeds the signing limit.')
            digest, count = hashlib.sha256(), 0
            with archive.open(entry) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    count += len(chunk); digest.update(chunk)
            require(count == entry.file_size, 'APK ZIP entry length differs.')
            if not jar_signature_entry(name):
                result[name] = {'sha256': digest.hexdigest(), 'bytes': count, 'directory': entry.is_dir()}
    require({'AndroidManifest.xml', 'classes.dex'} <= result.keys(), 'Required APK application payload is missing.')
    return result


def child_environment(environment, password=None):
    allowed = ('PATH', 'JAVA_HOME', 'HOME', 'LANG', 'LC_ALL', 'LC_CTYPE', 'TZ',
               'SYSTEMROOT', 'WINDIR', 'PATHEXT', 'TMP', 'TEMP', 'TMPDIR')
    result = {name: environment[name] for name in allowed if name in environment}
    if result.get('JAVA_HOME'):
        result['PATH'] = str(Path(result['JAVA_HOME']) / 'bin') + os.pathsep + result.get('PATH', '')
    if password is not None:
        result[PASSWORD_ENV] = password
    return result


def tool_run(command, environment):
    try:
        completed = subprocess.run(command, env=environment, stdin=subprocess.DEVNULL,
                                   capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        raise SigningError('Signing validation tool could not complete; private output suppressed.') from None
    require(completed.returncode == 0, 'Signing validation tool failed; private output suppressed.')
    # Tool output is parsed in memory; neither stdout nor stderr is logged.
    return completed.stdout


def package_identity(text):
    matches = re.findall(r"^package: name='([^'\r\n]+)' versionCode='([0-9]+)' versionName='([^'\r\n]+)'(?: |$)", text, re.M)
    require(len(matches) == 1, 'APK identity metadata is unavailable or ambiguous.')
    package, code, name = matches[0]
    require(package == APPLICATION_ID and 0 < int(code) <= 2100000000 and len(name) <= 256,
            'APK package identity or version is invalid.')
    return {'application_id': package, 'version_code': int(code), 'version_name': name}


def signature_identity(text, expected):
    signers = re.findall(r'^Signer #([0-9]+) certificate SHA-256 digest: ([0-9a-fA-F]{64})$', text, re.M)
    counts = re.findall(r'^Number of signers: ([0-9]+)$', text, re.M)
    require(counts == ['1'] and len(signers) == 1 and signers[0][0] == '1' and
            signers[0][1].lower() == expected, 'APK signer does not match the pinned certificate.')
    schemes = {}
    for version in ('v1', 'v2', 'v3', 'v4'):
        values = re.findall(r'^Verified using ' + version + r' scheme \([^\r\n]+\): (true|false)$', text, re.M)
        require(len(values) == 1, 'APK signing scheme verification is incomplete.')
        schemes[version] = values[0] == 'true'
    require(schemes == {'v1': False, 'v2': True, 'v3': True, 'v4': False} and
            not re.search(r'^Verified using v3\.1 scheme [^\r\n]+: true$', text, re.M),
            'APK signing schemes differ from the required policy.')
    return signers[0][1].lower(), schemes


def source_binding(path):
    if path is None:
        return None
    path = checked_path(path)
    require(path.stat().st_size <= 16 * 1024 * 1024, 'Source manifest is too large.')
    before = identity(path)
    data = json.loads(path.read_text())
    name, expected = data['source_archive'], data['source_sha256']
    require(isinstance(name, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*\.tar\.gz', name) and
            isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected), 'Source archive declaration is invalid.')
    archive = checked_path(path.parent / name)
    require(file_digest(archive) == expected and identity(path) == before, 'Source archive or manifest integrity differs.')
    return {'manifest_sha256': before[-1], 'source_archive': name, 'source_archive_sha256': expected}


def ci_metadata(environment):
    keys = ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')
    if not any(environment.get(name) for name in keys):
        return None
    require(all(environment.get(name) for name in keys), 'CI source metadata is incomplete.')
    require(re.fullmatch('[0-9a-f]{40}', environment[keys[0]]) and
            all(re.fullmatch('[1-9][0-9]{0,19}', environment[name]) for name in keys[1:]),
            'CI source metadata is invalid.')
    return {'source_commit': environment[keys[0]], 'run_id': environment[keys[1]], 'run_attempt': environment[keys[2]]}


def private_temp_root(environment):
    configured = environment.get('RUNNER_TEMP') or environment.get('TMPDIR')
    require(configured, 'RUNNER_TEMP or a private TMPDIR must be configured.')
    path = checked_path(configured, directory=True)
    info = path.stat()
    require(info.st_uid == os.getuid() and not (info.st_mode & 0o022), 'Signing temporary root must be owned and non-shared-writable.')
    if not environment.get('RUNNER_TEMP'):
        require(not (info.st_mode & 0o077), 'Fallback TMPDIR must be private.')
    return path


def publish_directory(source, destination):
    # Linux CI requires atomic no-replace publication, including a racing path.
    if sys.platform.startswith('linux'):
        libc = ctypes.CDLL(None, use_errno=True)
        rename = getattr(libc, 'renameat2', None)
        require(rename is not None, 'Atomic no-replace publication is unavailable.')
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1) != 0:
            raise OSError(ctypes.get_errno(), 'Atomic signing output publication failed.')
    else:
        raise SigningError('Atomic signing output publication requires a Linux host.')


def sign_apk(input_apk, output_dir, expected_certificate, apksigner=None, *, zipalign=None, aapt=None,
             expected_version_code=None, expected_version_name=None, source_manifest=None, environment=None):
    environment = dict(os.environ if environment is None else environment)
    try:
        require(sys.platform.startswith('linux'), 'Post-build signing requires a Linux host.')
        encoded, password = environment.get(KEYSTORE_ENV, ''), environment.get(PASSWORD_ENV, '')
        require(isinstance(encoded, str) and 0 < len(encoded) <= ((MAX_KEYSTORE_BYTES + 2) // 3) * 4 and
                isinstance(password, str) and 0 < len(password) <= 4096 and '\x00' not in password,
                'Signing credentials are missing or outside the supported bounds.')
        try:
            key_bytes = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            raise SigningError('Keystore Base64 is invalid.') from None
        require(0 < len(key_bytes) <= MAX_KEYSTORE_BYTES and base64.b64encode(key_bytes).decode() == encoded,
                'Keystore Base64 is not bounded canonical encoding.')
        source = checked_path(input_apk)
        require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,200}\.apk', source.name) and
                0 < source.stat().st_size <= MAX_APK_BYTES, 'Input APK name or size is invalid.')
        output = checked_path(output_dir, existing=False)
        parent = checked_path(output.parent, directory=True)
        require(not output.exists(), 'Signing output directory must be fresh.')
        pin = checked_path(expected_certificate)
        require(pin.stat().st_size <= 256, 'Certificate pin file is invalid.')
        pin_text = pin.read_text(encoding='ascii')
        require(re.fullmatch(r'[0-9a-fA-F]{64}(?:\r?\n)?', pin_text), 'Certificate pin must contain exactly one SHA-256.')
        expected = pin_text.strip().lower()
        sdk = Path(environment.get('ANDROID_HOME', '')) / 'build-tools/35.0.0'
        tools = {}
        for name, explicit in (('apksigner', apksigner), ('zipalign', zipalign), ('aapt', aapt)):
            require(explicit is not None or environment.get('ANDROID_HOME'), 'Explicit signing tools or ANDROID_HOME are required.')
            tools[name] = Path(explicit or sdk / name).resolve(strict=True)
            require(tools[name].is_file() and os.access(tools[name], os.X_OK), 'Required Android signing tool is unavailable.')
        public_environment = child_environment(environment)
        original = identity(source)
        original_pin = identity(pin)
        provenance = source_binding(source_manifest)
        ci = ci_metadata(environment)
        temporary_root = private_temp_root(environment)
        with tempfile.TemporaryDirectory(prefix='.android-signing-', dir=parent) as staging_name:
            staging = Path(staging_name); staging.chmod(0o700)
            snapshot = staging / '.unsigned-input.apk'
            with source.open('rb') as stream, snapshot.open('xb') as target:
                shutil.copyfileobj(stream, target, 1024 * 1024)
            require(file_digest(snapshot) == original[-1], 'Input APK changed while being staged.')
            payload = zip_payload(snapshot)
            before_package = package_identity(tool_run([str(tools['aapt']), 'dump', 'badging', str(snapshot)], public_environment))
            require(expected_version_code is None or before_package['version_code'] == int(expected_version_code), 'APK versionCode differs from the expected source.')
            require(expected_version_name is None or before_package['version_name'] == expected_version_name, 'APK versionName differs from the expected source.')
            alignment = [str(tools['zipalign']), '-c', '-P', '16', '4']
            tool_run([*alignment, str(snapshot)], public_environment)
            signed = staging / source.name
            with tempfile.TemporaryDirectory(prefix='heaven-android-key-', dir=temporary_root) as key_directory:
                private = Path(key_directory); private.chmod(0o700)
                key = private / 'signing.p12'
                descriptor = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(descriptor, 'wb') as stream:
                    stream.write(key_bytes)
                key.chmod(0o600)
                command = [str(tools['apksigner']), 'sign', '--ks', str(key), '--ks-type', 'PKCS12',
                    '--ks-key-alias', 'heavenbench', '--ks-pass', 'env:' + PASSWORD_ENV, '--key-pass', 'env:' + PASSWORD_ENV,
                    '--min-sdk-version', '26', '--v1-signing-enabled', 'false', '--v2-signing-enabled', 'true',
                    '--v3-signing-enabled', 'true', '--v4-signing-enabled', 'false', '--out', str(signed), str(snapshot)]
                tool_run(command, child_environment(environment, password))
            require(signed.is_file() and not signed.is_symlink() and signed.stat().st_size <= MAX_APK_BYTES,
                    'Signed APK output is invalid.')
            require(not os.path.samefile(signed, source) and not os.path.samefile(signed, snapshot),
                    'Signed APK must not alias an input file.')
            actual, schemes = signature_identity(tool_run([str(tools['apksigner']), 'verify', '--verbose', '--print-certs',
                '--min-sdk-version', '26', str(signed)], public_environment), expected)
            require(zip_payload(signed) == payload, 'Signing changed non-signature APK payload.')
            after_package = package_identity(tool_run([str(tools['aapt']), 'dump', 'badging', str(signed)], public_environment))
            require(after_package == before_package, 'Signing changed APK package identity.')
            tool_run([*alignment, str(signed)], public_environment)
            require({p.name for p in staging.iterdir()} == {snapshot.name, signed.name}, 'Signing created unexpected sidecar outputs.')
            require(identity(source) == original and identity(pin) == original_pin and source_binding(source_manifest) == provenance,
                    'Original APK, public pin or source inputs changed.')
            snapshot.unlink()
            digest = file_digest(signed)
            receipt = {'schema_version': 1, 'mode': 'pinned-certificate/post-build', 'status': 'passed',
                'expected_certificate_sha256': expected, 'actual_certificate_sha256': actual,
                'signing_schemes': schemes, 'key_alias': 'heavenbench', 'minimum_sdk': 26,
                'input_apk': {'file': source.name, 'sha256': original[-1]},
                'output_apk': {'file': signed.name, 'bytes': signed.stat().st_size, 'sha256': digest},
                'package_identity': before_package, 'alignment': {'input_16k_verified': True, 'output_16k_verified': True},
                'payload_verification': {'entries_verified': len(payload), 'uncompressed_bytes': sum(p['bytes'] for p in payload.values()),
                    'inventory_sha256': hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest(),
                    'rule': 'Exact uncompressed ZIP entry payloads except direct reserved JAR signature metadata.'},
                'source': provenance, 'ci': ci,
                'scope': 'Pinned certificate and post-build payload/identity/alignment verification; CI credential persistence and device execution are separate.'}
            (staging / (source.name + '.sha256')).write_text(digest + '  ' + source.name + '\n')
            (staging / 'signing-certificate.json').write_text(json.dumps(receipt, indent=2) + '\n')
            for path in staging.iterdir(): path.chmod(0o644)
            publish_directory(staging, output)
            return receipt
    except SigningError:
        raise
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, zipfile.BadZipFile, zlib.error, subprocess.SubprocessError):
        raise SigningError('APK signing failed; private tool output suppressed.') from None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', dest='input_apk', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--expected-certificate', type=Path, required=True)
    for name in ('apksigner', 'zipalign', 'aapt', 'source-manifest'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--expected-version-code', type=int)
    parser.add_argument('--expected-version-name')
    args = parser.parse_args(argv)
    try:
        receipt = sign_apk(**vars(args))
    except SigningError as error:
        print('Android signing rejected: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps(receipt, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
