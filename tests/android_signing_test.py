#!/usr/bin/env python3
"""CPU-only signing contracts; optional real APK/throwaway-key integration."""
import argparse
import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('android_signer', ROOT / 'scripts/sign_android_apk.py')
SIGNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SIGNER)
PIN = '12' * 32
PASSWORD = 'test-password-private-marker'
KEY = b'not a real PKCS12; CPU mock only'


def verification(pin=PIN, v3=True):
    return ('Verifies\nVerified using v1 scheme (JAR signing): false\n'
            'Verified using v2 scheme (APK Signature Scheme v2): true\n'
            f'Verified using v3 scheme (APK Signature Scheme v3): {str(v3).lower()}\n'
            'Verified using v3.1 scheme (APK Signature Scheme v3.1): false\n'
            'Verified using v4 scheme (APK Signature Scheme v4): false\n'
            'Number of signers: 1\nSigner #1 certificate SHA-256 digest: ' + pin + '\n')


@unittest.skipUnless(sys.platform.startswith('linux'), 'Post-build signing runs on Linux CI.')
class AndroidSigningTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='android-signing-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.private = self.root / 'private'
        self.private.mkdir(mode=0o700)
        self.apk = self.root / 'HeavenBench-fixture.apk'
        self.write_apk()
        self.pin = self.root / 'pin.sha256'
        self.pin.write_text(PIN + '\n')
        # Execution is mocked; use an actual executable even on noexec /tmp.
        self.tool = Path(sys.executable).resolve()
        self.env = {'RUNNER_TEMP': str(self.private), 'PATH': os.environ.get('PATH', ''),
                    'HEAVEN_ANDROID_KEYSTORE_BASE64': base64.b64encode(KEY).decode(),
                    'HEAVEN_ANDROID_KEYSTORE_PASSWORD': PASSWORD}
        self.calls = []
        self.output = self.root / 'signed'

    def write_apk(self, entries=None):
        entries = entries or [('AndroidManifest.xml', b'binary manifest fixture'), ('classes.dex', b'dex fixture'),
                             ('lib/arm64-v8a/libexample.so', b'ELF fixture'), ('META-INF/services/X.RSA', b'protected service'),
                             ('META-INF/NOTICE.MF', b'protected notice'), ('META-INF/CERT.SF', b'old signature metadata')]
        with zipfile.ZipFile(self.apk, 'w') as archive:
            for name, content in entries:
                archive.writestr(name, content)

    def fake_tool(self, command, **kwargs):
        self.calls.append((command, kwargs))
        environment = kwargs['env']
        self.assertNotIn('HEAVEN_ANDROID_KEYSTORE_BASE64', environment)
        self.assertNotIn('ANDROID_SIGNING_KEYSTORE_BASE64', environment)
        self.assertNotIn('GITHUB_TOKEN', environment)
        self.assertNotIn(PASSWORD, ' '.join(command))
        self.assertEqual(kwargs['stdin'], subprocess.DEVNULL)
        if command[1] in ('dump', '-c'):
            self.assertNotIn('HEAVEN_ANDROID_KEYSTORE_PASSWORD', environment)
            text = "package: name='app.zetta.heavenbench' versionCode='5' versionName='1.1.1' platformBuildVersionName='35'\n" if command[1] == 'dump' else ''
            return subprocess.CompletedProcess(command, 0, text, '')
        if command[1] == 'sign':
            key = Path(command[command.index('--ks') + 1])
            self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(key.parent.stat().st_mode), 0o700)
            self.assertTrue(key.parent.is_relative_to(self.private))
            self.assertEqual(key.read_bytes(), KEY)
            self.assertEqual(environment['HEAVEN_ANDROID_KEYSTORE_PASSWORD'], PASSWORD)
            self.assertEqual(command[command.index('--ks-pass') + 1], 'env:HEAVEN_ANDROID_KEYSTORE_PASSWORD')
            self.assertEqual(command[command.index('--key-pass') + 1], 'env:HEAVEN_ANDROID_KEYSTORE_PASSWORD')
            self.assertEqual(command[command.index('--ks-key-alias') + 1], 'heavenbench')
            self.assertEqual(command[command.index('--ks-type') + 1], 'PKCS12')
            for version, expected in [('v1', 'false'), ('v2', 'true'), ('v3', 'true'), ('v4', 'false')]:
                self.assertEqual(command[command.index('--' + version + '-signing-enabled') + 1], expected)
            self.assertEqual(command[command.index('--min-sdk-version') + 1], '26')
            shutil.copyfile(command[-1], command[command.index('--out') + 1])
            return subprocess.CompletedProcess(command, 0, '', '')
        self.assertEqual(command[1], 'verify')
        self.assertNotIn('HEAVEN_ANDROID_KEYSTORE_PASSWORD', environment)
        return subprocess.CompletedProcess(command, 0, verification(), '')

    def sign(self, callback=None, **changes):
        values = dict(input_apk=self.apk, output_dir=self.output, expected_certificate=self.pin,
                      apksigner=self.tool, zipalign=self.tool, aapt=self.tool, environment=self.env)
        values.update(changes)
        with patch.object(SIGNER.subprocess, 'run', side_effect=callback or self.fake_tool):
            return SIGNER.sign_apk(**values)

    def assert_clean_failure(self, callback=None, **changes):
        with self.assertRaises(SIGNER.SigningError):
            self.sign(callback, **changes)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.private.iterdir()), [])
        self.assertFalse(any(p.name.startswith('.android-signing-') for p in self.root.iterdir()))

    def test_private_credentials_payload_pin_and_public_receipt(self):
        before = self.apk.read_bytes()
        report = self.sign()
        self.assertEqual(self.apk.read_bytes(), before)
        self.assertEqual(report['mode'], 'pinned-certificate/post-build')
        self.assertEqual(report['expected_certificate_sha256'], PIN)
        self.assertEqual(report['actual_certificate_sha256'], PIN)
        self.assertEqual(report['payload_verification']['entries_verified'], 5)
        self.assertEqual(report['signing_schemes'], {'v1': False, 'v2': True, 'v3': True, 'v4': False})
        self.assertEqual(set(p.name for p in self.output.iterdir()), {self.apk.name, self.apk.name + '.sha256', 'signing-certificate.json'})
        self.assertEqual(list(self.private.iterdir()), [])
        public = (self.output / 'signing-certificate.json').read_text()
        self.assertNotIn(PASSWORD, public)
        self.assertNotIn(self.env['HEAVEN_ANDROID_KEYSTORE_BASE64'], public)

    def test_missing_and_invalid_credentials_fail_without_output(self):
        for key, value in [('HEAVEN_ANDROID_KEYSTORE_BASE64', ''), ('HEAVEN_ANDROID_KEYSTORE_PASSWORD', ''),
                           ('HEAVEN_ANDROID_KEYSTORE_BASE64', 'bad!'), ('HEAVEN_ANDROID_KEYSTORE_BASE64', 'YQ==\n'),
                           ('HEAVEN_ANDROID_KEYSTORE_BASE64', 'YR==')]:
            with self.subTest(key=key, value=value):
                env = dict(self.env); env[key] = value
                self.assert_clean_failure(environment=env)
        with patch.object(SIGNER, 'MAX_KEYSTORE_BYTES', 1):
            self.assert_clean_failure()
        self.assertEqual(self.calls, [])

    def test_wrong_pin_and_invalid_pin_fail(self):
        self.pin.write_text('not a sha256\n')
        self.assert_clean_failure()
        self.pin.write_text('34' * 32 + '\n')
        self.assert_clean_failure()

    def test_tool_failure_is_redacted_and_cleans_credentials(self):
        def failure(command, **kwargs):
            self.fake_tool(command, **kwargs)
            return subprocess.CompletedProcess(command, 1, PASSWORD, self.env['HEAVEN_ANDROID_KEYSTORE_BASE64'])
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()) as err:
            self.assert_clean_failure(failure)
        self.assertEqual(out.getvalue() + err.getvalue(), '')

    def test_payload_mutation_and_duplicate_output_rejected(self):
        for name in ['classes.dex', 'META-INF/services/X.RSA', 'META-INF/NOTICE.MF']:
            def mutate(command, **kwargs):
                result = self.fake_tool(command, **kwargs)
                if command[1] == 'sign':
                    target = Path(command[command.index('--out') + 1])
                    with zipfile.ZipFile(target) as archive:
                        entries = [(i, archive.read(i)) for i in archive.infolist()]
                    with zipfile.ZipFile(target, 'w') as archive:
                        for item, content in entries:
                            archive.writestr(item, b'changed' if item.filename == name else content)
                return result
            with self.subTest(name=name):
                self.assert_clean_failure(mutate)

    def test_reserved_signature_metadata_may_change(self):
        def change_signature(command, **kwargs):
            result = self.fake_tool(command, **kwargs)
            if command[1] == 'sign':
                target = Path(command[command.index('--out') + 1])
                with zipfile.ZipFile(target) as archive:
                    entries = [(i, archive.read(i)) for i in archive.infolist() if not SIGNER.jar_signature_entry(i.filename)]
                with zipfile.ZipFile(target, 'w') as archive:
                    for item, content in entries:
                        archive.writestr(item, content)
            return result
        self.assertEqual(self.sign(change_signature)['payload_verification']['entries_verified'], 5)

    def test_signature_exclusions_match_precise_reserved_namespace(self):
        for name in ['META-INF/MANIFEST.MF', 'META-INF/CERT.SF', 'META-INF/CERT.rSa', 'META-INF/SIG-CUSTOM']:
            self.assertTrue(SIGNER.jar_signature_entry(name), name)
        for name in ['meta-inf/CERT.RSA', 'META-INF/services/X.RSA', 'META-INF/NOTICE.MF', 'assets/META-INF/CERT.RSA']:
            self.assertFalse(SIGNER.jar_signature_entry(name), name)

    def test_alignment_and_application_identity_fail_closed(self):
        for which in ['alignment', 'application', 'version', 'changed-after']:
            identities = 0
            def failure(command, **kwargs):
                nonlocal identities
                result = self.fake_tool(command, **kwargs)
                if which == 'alignment' and command[1] == '-c':
                    return subprocess.CompletedProcess(command, 1, '', '')
                if command[1] == 'dump':
                    identities += 1
                    if which == 'application' or (which == 'changed-after' and identities == 2):
                        return subprocess.CompletedProcess(command, 0, result.stdout.replace('app.zetta.heavenbench', 'app.example.other'), '')
                    if which == 'version':
                        return subprocess.CompletedProcess(command, 0, result.stdout.replace("versionCode='5'", "versionCode='0'"), '')
                return result
            with self.subTest(which=which): self.assert_clean_failure(failure)
        self.assert_clean_failure(expected_version_code=6)
        self.assert_clean_failure(expected_version_name='1.1.0')

    def test_source_manifest_and_ci_metadata_are_public_and_bound(self):
        archive = self.root / 'HeavenBench-source.tar.gz'; archive.write_bytes(b'not extracted or executed')
        manifest = self.root / 'source-manifest.json'
        manifest.write_text(json.dumps({'source_archive': archive.name, 'source_sha256': SIGNER.file_digest(archive)}))
        env = dict(self.env, GITHUB_SHA='a' * 40, GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='2')
        report = self.sign(source_manifest=manifest, environment=env)
        self.assertEqual(report['source']['manifest_sha256'], SIGNER.file_digest(manifest))
        self.assertEqual(report['ci'], {'source_commit':'a' * 40, 'run_id':'123', 'run_attempt':'2'})
        shutil.rmtree(self.output)
        archive.write_bytes(b'changed')
        self.assert_clean_failure(source_manifest=manifest)
        manifest.write_text(json.dumps({'source_archive':'../outside.tar.gz','source_sha256':'a' * 64}))
        self.assert_clean_failure(source_manifest=manifest)
        self.assert_clean_failure(environment=dict(self.env,GITHUB_SHA='a' * 40))
        self.assert_clean_failure(environment=dict(env,GITHUB_RUN_ID='not-decimal'))

    def test_invalid_zip_and_original_change_do_not_publish(self):
        self.apk.write_bytes(b'not a ZIP')
        self.assert_clean_failure()
        self.write_apk()
        def change_original(command, **kwargs):
            result=self.fake_tool(command,**kwargs)
            if command[1]=='sign':self.apk.write_bytes(b'changed externally')
            return result
        self.assert_clean_failure(change_original)

    def test_corrupt_deflate_payload_is_rejected_without_raw_exception(self):
        with zipfile.ZipFile(self.apk,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('AndroidManifest.xml',b'manifest')
            archive.writestr('classes.dex',b'dex'*100)
        with zipfile.ZipFile(self.apk) as archive:
            item=archive.getinfo('classes.dex')
            offset=item.header_offset+30+len(item.filename.encode())+len(item.extra)
        content=bytearray(self.apk.read_bytes());content[offset]=255;self.apk.write_bytes(content)
        self.assert_clean_failure()

    def test_unsafe_zip_entries_and_duplicate_input_rejected(self):
        for bad in ['../escape', '/absolute', 'a\\b', 'a//b', 'a/./b', 'C:/drive', 'a\x00tail']:
            with self.subTest(path=bad):
                # zipfile truncates NUL names while writing; raw ambiguity is covered separately.
                if '\x00' in bad:
                    continue
                self.write_apk([('AndroidManifest.xml', b'manifest'), ('classes.dex', b'dex'), (bad, b'bad')])
                self.assert_clean_failure()
        self.write_apk([('AndroidManifest.xml', b'manifest'), ('classes.dex', b'one'), ('classes.dex', b'two')])
        self.assert_clean_failure()
        symlink = zipfile.ZipInfo('link'); symlink.create_system = 3; symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(self.apk, 'w') as archive: archive.writestr(symlink, 'target')
        self.assert_clean_failure()

    def test_signature_schemes_multiple_signers_and_idsig_fail(self):
        for text in [verification(v3=False), verification() + 'Signer #2 certificate SHA-256 digest: ' + PIN + '\n']:
            def invalid(command, **kwargs):
                result = self.fake_tool(command, **kwargs)
                return subprocess.CompletedProcess(command, 0, text, '') if command[1] == 'verify' else result
            self.assert_clean_failure(invalid)
        def idsig(command, **kwargs):
            result = self.fake_tool(command, **kwargs)
            if command[1] == 'sign': Path(command[command.index('--out') + 1] + '.idsig').write_bytes(b'forbidden')
            return result
        self.assert_clean_failure(idsig)

    def test_input_output_symlinks_and_existing_destination_fail(self):
        alias = self.root / 'alias.apk'; alias.symlink_to(self.apk)
        self.assert_clean_failure(input_apk=alias)
        link = self.root / 'out-link'; link.symlink_to(self.private, target_is_directory=True)
        self.assert_clean_failure(output_dir=link)
        self.output.mkdir(); (self.output / 'sentinel').write_text('preserve')
        with self.assertRaises(SIGNER.SigningError): self.sign()
        self.assertEqual((self.output / 'sentinel').read_text(), 'preserve')

    def test_no_clobber_publication_race_preserves_other_directory(self):
        def race(source, destination):
            destination.mkdir(); (destination / 'sentinel').write_text('preserve')
            raise FileExistsError('race')
        with patch.object(SIGNER, 'publish_directory', side_effect=race), self.assertRaises(SIGNER.SigningError): self.sign()
        self.assertEqual((self.output / 'sentinel').read_text(), 'preserve')
        self.assertEqual(list(self.private.iterdir()), [])

    def test_actual_atomic_publisher_refuses_existing_directory(self):
        staged=self.root/'staged';staged.mkdir();(staged/'payload').write_text('new')
        self.output.mkdir();(self.output/'sentinel').write_text('preserve')
        with self.assertRaises(OSError):SIGNER.publish_directory(staged,self.output)
        self.assertEqual((self.output/'sentinel').read_text(),'preserve')
        self.assertEqual((staged/'payload').read_text(),'new')

    def test_output_hardlink_to_original_is_rejected_before_publication(self):
        before=self.apk.read_bytes()
        def alias(command,**kwargs):
            result=self.fake_tool(command,**kwargs)
            if command[1]=='sign':
                target=Path(command[command.index('--out')+1]);target.unlink();os.link(self.apk,target)
            return result
        self.assert_clean_failure(alias)
        self.assertEqual(self.apk.read_bytes(),before)

    def test_raw_nul_zip_name_and_file_directory_aliases_rejected(self):
        self.write_apk([('AndroidManifest.xml',b'manifest'),('classes.dex',b'dex'),('N0T_NULL',b'bad')])
        self.apk.write_bytes(self.apk.read_bytes().replace(b'N0T_NULL',b'N\x00T_NULL'))
        self.assert_clean_failure()
        self.write_apk([('AndroidManifest.xml',b'manifest'),('classes.dex',b'dex'),('alias',b'file'),('alias/',b'')])
        self.assert_clean_failure()

    def test_cli_never_discloses_tool_exceptions(self):
        argv = ['--input', str(self.apk), '--output-dir', str(self.output), '--expected-certificate', str(self.pin), '--apksigner', str(self.tool), '--zipalign', str(self.tool), '--aapt', str(self.tool)]
        with patch.dict(os.environ, self.env, clear=True), patch.object(SIGNER.subprocess, 'run', side_effect=OSError(PASSWORD)), contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(SIGNER.main(argv), 1)
        self.assertNotIn(PASSWORD, err.getvalue())
        self.assertFalse(self.output.exists())


def integration(arguments):
    """Generate only disposable fixture identities and never print private material."""
    with tempfile.TemporaryDirectory(prefix='android-signing-integration-') as temporary:
        root = Path(temporary); root.chmod(0o700)
        env = dict(os.environ, RUNNER_TEMP=str(root))
        password = 'fixture-' + os.urandom(24).hex()
        env['HEAVEN_ANDROID_KEYSTORE_PASSWORD'] = password
        original = SIGNER.file_digest(arguments.integration_apk)
        fingerprints = []
        for number in range(2):
            store = root / f'fixture-{number}.p12'
            command = [str(arguments.keytool), '-genkeypair', '-alias', 'heavenbench', '-storetype', 'PKCS12',
                '-keystore', str(store), '-storepass:env', 'HEAVEN_ANDROID_KEYSTORE_PASSWORD', '-keypass:env',
                'HEAVEN_ANDROID_KEYSTORE_PASSWORD', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '2',
                '-dname', 'CN=Disposable HeavenBench signing test', '-noprompt']
            done = subprocess.run(command, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
            if done.returncode: raise RuntimeError('Throwaway keytool generation failed; private output suppressed')
            store.chmod(0o600)
            certificate = subprocess.run([str(arguments.keytool), '-exportcert', '-alias', 'heavenbench', '-storetype', 'PKCS12', '-keystore', str(store), '-storepass:env', 'HEAVEN_ANDROID_KEYSTORE_PASSWORD'], env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=60)
            if certificate.returncode: raise RuntimeError('Throwaway public-certificate export failed')
            fingerprints.append(hashlib.sha256(certificate.stdout).hexdigest())
        pin = root / 'expected.sha256'; pin.write_text(fingerprints[0] + '\n')
        env['HEAVEN_ANDROID_KEYSTORE_BASE64'] = base64.b64encode((root / 'fixture-0.p12').read_bytes()).decode()
        reports = [SIGNER.sign_apk(arguments.integration_apk, root/f'signed-{i}', pin, arguments.apksigner,
                   zipalign=arguments.zipalign, aapt=arguments.aapt, environment=env) for i in range(2)]
        assert all(r['actual_certificate_sha256'] == fingerprints[0] for r in reports)
        env['HEAVEN_ANDROID_KEYSTORE_BASE64'] = base64.b64encode((root / 'fixture-1.p12').read_bytes()).decode()
        try: SIGNER.sign_apk(arguments.integration_apk, root/'wrong-key', pin, arguments.apksigner,
                            zipalign=arguments.zipalign, aapt=arguments.aapt, environment=env)
        except SIGNER.SigningError: pass
        else: raise AssertionError('Different throwaway identity accepted')
        assert not (root/'wrong-key').exists()
        original_key = base64.b64encode((root/'fixture-0.p12').read_bytes()).decode()
        missing = dict(env); missing.pop('HEAVEN_ANDROID_KEYSTORE_BASE64')
        rejected = [('missing-secret', missing),
            ('invalid-base64', dict(env, HEAVEN_ANDROID_KEYSTORE_BASE64='invalid!')),
            ('invalid-pkcs12', dict(env, HEAVEN_ANDROID_KEYSTORE_BASE64=base64.b64encode(b'not PKCS12').decode())),
            ('wrong-password', dict(env, HEAVEN_ANDROID_KEYSTORE_BASE64=original_key, HEAVEN_ANDROID_KEYSTORE_PASSWORD=password+'-wrong'))]
        for label, candidate in rejected:
            try: SIGNER.sign_apk(arguments.integration_apk, root/label, pin, arguments.apksigner,
                                zipalign=arguments.zipalign, aapt=arguments.aapt, environment=candidate)
            except SIGNER.SigningError: pass
            else: raise AssertionError('Invalid throwaway secret accepted')
            assert not (root/label).exists()
        assert not any(p.name.startswith('heaven-android-key-') for p in root.iterdir())
        assert SIGNER.file_digest(arguments.integration_apk) == original
        print(json.dumps({'integration':'passed','identity_repeated':True,'different_identity_rejected':True,
            'missing_invalid_secret_and_real_tool_failure_cleanup':True,
            'payload_entries_verified':reports[0]['payload_verification']['entries_verified'],
            'signing_schemes':reports[0]['signing_schemes'],'original_unchanged':True,'throwaway_keys_only':True}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--integration-apk', type=Path)
    parser.add_argument('--apksigner', type=Path)
    parser.add_argument('--keytool', type=Path)
    parser.add_argument('--zipalign', type=Path)
    parser.add_argument('--aapt', type=Path)
    options, remaining = parser.parse_known_args()
    result = unittest.main(argv=[sys.argv[0], *remaining], exit=False).result
    if not result.wasSuccessful(): raise SystemExit(1)
    if options.integration_apk:
        if not all((options.apksigner, options.keytool, options.zipalign, options.aapt)):
            parser.error('--apksigner, --keytool, --zipalign and --aapt required for integration')
        integration(options)
