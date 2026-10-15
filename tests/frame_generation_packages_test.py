#!/usr/bin/env python3
"""Experimental packaging policy tests; no SDK download, renderer build or GPU."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_metadata', ROOT / 'scripts/frame_generation_package_metadata.py')
metadata = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metadata)


class PackageMetadataTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='heaven SDK package ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'source'
        self.fg = self.source / 'benchmark/src/rhi/frame_generation'
        self.fg.mkdir(parents=True)
        (self.source / 'benchmark/CMakeLists.txt').write_text('project(HeavenBench VERSION 1.2.3 LANGUAGES CXX)\n')
        (self.fg / 'fsr31-source.sha256').write_text('fixture reviewed source lock\n')
        self.build = self.root / 'build'; self.build.mkdir()
        self.stage = self.root / 'stage'; self.stage.mkdir()
        self.binary = self.stage / 'heaven_benchmark.exe'; self.binary.write_bytes(b'fixture executable')
        self.licenses = self.stage / 'licenses'
        self.sdk = self.root / 'SDK source'; self.sdk.mkdir()
        self.headers = self.root / 'verified headers'; self.headers.mkdir()
        for relative, name in [('LICENSE.txt', 'FidelityFX-SDK.txt'),
                ('sdk/tools/ffx_shader_compiler/libs/SPIRV-Reflect/LICENSE', 'SPIRV-Reflect.txt')]:
            source = self.sdk / relative; source.parent.mkdir(parents=True, exist_ok=True); source.write_text(name)
            output = self.licenses / 'repository/packaging/licenses' / name
            output.parent.mkdir(parents=True, exist_ok=True); output.write_bytes(source.read_bytes())
        (self.headers / 'license.txt').write_text('verified Streamline notice')
        (self.headers / 'heavenbench-source.json').write_text('{"commit":"fixture","archive_sha256":"fixture"}')
        output = self.licenses / 'dependencies/Streamline/license.txt'; output.parent.mkdir(parents=True)
        output.write_bytes((self.headers / 'license.txt').read_bytes())
        shader = self.build / 'src/rhi/frame_generation/fsr31_shader_blobs.json'; shader.parent.mkdir(parents=True)
        shader.write_text(json.dumps({'spirv_validated': True, 'shaders': [{}] * 102}))
        self.archive = self.root / 'source.tar.gz'; self.archive.write_bytes(b'exact archive fixture')
        self.manifest = self.root / 'source-manifest.json'
        self.manifest.write_text(json.dumps({'source_archive': self.archive.name, 'source_sha256': metadata.sha(self.archive),
            'files': {'benchmark/CMakeLists.txt': metadata.sha(self.source / 'benchmark/CMakeLists.txt')}}))
        self.cache('ON', 'OFF')
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(metadata, 'ROOT', self.source).start()
        mock.patch.object(metadata, 'FG', self.fg).start()
        self.verify = mock.patch.object(metadata, 'verify').start()

    def cache(self, fsr, dlss):
        (self.build / 'CMakeCache.txt').write_text(f'CMAKE_BUILD_TYPE:STRING=Release\nHEAVEN_CPU_ARCH:STRING=portable\n'
            f'CMAKE_HOME_DIRECTORY:INTERNAL={self.source / "benchmark"}\n'
            f'HEAVEN_ENABLE_FSR3:BOOL={fsr}\nHEAVEN_ENABLE_DLSS:BOOL={dlss}\n'
            f'HEAVEN_FSR31_SOURCE:PATH={self.sdk}\nHEAVEN_STREAMLINE_HEADERS:PATH={self.headers}\n')

    def write(self, providers='fsr3', platform='linux'):
        return metadata.write_metadata(self.build, self.stage, self.binary, self.licenses, self.manifest, providers, platform)

    def test_fsr_label_binary_source_and_validator_bindings(self):
        tag = self.write(); payload = json.loads((self.stage / 'experimental-frame-generation.json').read_text())
        self.assertEqual(tag, 'experimental-fsr3-' + metadata.sha(self.archive)[:12])
        self.assertEqual(payload['binary_sha256'], metadata.sha(self.binary))
        self.assertEqual(payload['default_frame_generation'], 'off')
        self.assertTrue(payload['sdk']['fsr3']['spirv_validated'])
        self.assertEqual(payload['sdk']['fsr3']['shader_permutations'], 102)
        self.assertIn('--lock', self.verify.call_args.args[0])
        self.assertIn('not qualified', payload['acceptance_scope'])

    def test_dlss_and_combined_labels_have_no_runtime_payload(self):
        for providers, fsr in [('dlss', 'OFF'), ('both', 'ON')]:
            with self.subTest(providers=providers):
                self.cache(fsr, 'ON'); self.write(providers, 'windows')
                payload = json.loads((self.stage / 'experimental-frame-generation.json').read_text())
                self.assertEqual(payload['providers_built'], providers)
                self.assertIn('not included', payload['sdk']['dlss']['runtime_plugins'])
                self.assertIn('--verify-only', self.verify.call_args.args[0])
                self.assertIn('HEAVEN_DLSS_PLUGIN_DIR', (self.stage / 'EXPERIMENTAL-FRAME-GENERATION.txt').read_text())

    def test_disabled_or_mismatched_providers_fail(self):
        self.cache('OFF', 'OFF')
        with self.assertRaisesRegex(ValueError, 'does not match'): self.write()
        self.cache('ON', 'ON')
        with self.assertRaisesRegex(ValueError, 'does not match'): self.write()
        self.cache('OFF', 'ON')
        with self.assertRaisesRegex(ValueError, 'does not match'): self.write('dlss', 'linux')

    def test_nvidia_plugins_are_rejected_even_when_nested(self):
        for name in ['sl.interposer.dll', 'sl.common.DLL', 'nvngx_dlssg.dll']:
            with self.subTest(name=name):
                plugin = self.stage / 'nested' / name; plugin.parent.mkdir(exist_ok=True); plugin.write_bytes(b'not distributable')
                with self.assertRaisesRegex(ValueError, 'bring-your-own'): self.write()
                plugin.unlink()

    def test_source_changes_and_archive_corruption_fail_closed(self):
        self.archive.write_bytes(b'corrupt source archive')
        with self.assertRaisesRegex(ValueError, 'checksum'): self.write()
        self.archive.write_bytes(b'exact archive fixture')
        (self.source / 'benchmark/CMakeLists.txt').write_text('edited after source snapshot')
        with self.assertRaisesRegex(ValueError, 'Source changed'): self.write()

    def test_missing_or_changed_sdk_notices_fail(self):
        notice = self.licenses / 'repository/packaging/licenses/FidelityFX-SDK.txt'
        notice.write_text('wrong notice')
        with self.assertRaisesRegex(ValueError, 'notice differs'): self.write()
        notice.unlink()
        with self.assertRaises(FileNotFoundError): self.write()

    def test_unvalidated_or_incomplete_shader_permutations_fail(self):
        shader = self.build / 'src/rhi/frame_generation/fsr31_shader_blobs.json'
        for receipt in [{'spirv_validated': False, 'shaders': [{}] * 102}, {'spirv_validated': True, 'shaders': [{}] * 101}]:
            shader.write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, '102 FSR shader'): self.write()

    def test_sdk_verification_failure_prevents_metadata(self):
        self.verify.side_effect = ValueError('SDK source drift')
        with self.assertRaisesRegex(ValueError, 'SDK source drift'): self.write()
        self.assertFalse((self.stage / 'experimental-frame-generation.json').exists())


class RepositorySourceExportTest(unittest.TestCase):
    def test_worktree_edits_included_and_ignored_plugins_excluded(self):
        with tempfile.TemporaryDirectory(prefix='heaven source policy ') as temporary:
            directory = Path(temporary); source = directory / 'repo'; source.mkdir()
            def git(*args):
                return subprocess.run(['git', '-C', str(source), *args], check=True, text=True, capture_output=True)
            git('init', '-q')
            (source / 'benchmark').mkdir(); (source / 'scripts').mkdir()
            (source / 'benchmark/CMakeLists.txt').write_text('project(HeavenBench VERSION 1.2.3 LANGUAGES CXX)\n')
            (source / '.gitignore').write_text('build/\n*.dll\n*.ung\n')
            (source / 'tracked.cpp').write_text('before')
            (source / 'removed.cpp').write_text('removed from current source')
            (source / 'scripts/export_build_sources.py').write_bytes((ROOT / 'scripts/export_build_sources.py').read_bytes())
            git('add', '.'); git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture')
            (source / 'tracked.cpp').write_text('current local edit')
            (source / 'removed.cpp').unlink()
            (source / 'new.cpp').write_text('new source used by build')
            (source / 'sl.interposer.dll').write_bytes(b'private plugin')
            (source / 'data.ung').write_bytes(b'original assets')
            output = directory / 'packages'
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/export_repository_sources.py'), '--source-root', str(source),
                '--output-dir', str(output), '--platform', 'windows-experimental-dlss'], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = json.loads((output / 'source-manifest.json').read_text())
            self.assertIn('tracked.cpp', manifest['files']); self.assertIn('new.cpp', manifest['files'])
            self.assertNotIn('sl.interposer.dll', manifest['files']); self.assertNotIn('data.ung', manifest['files'])
            self.assertNotIn('removed.cpp', manifest['files'])
            self.assertTrue(manifest['git_worktree_status'])
            self.assertEqual(manifest['source_sha256'], hashlib.sha256((output / manifest['source_archive']).read_bytes()).hexdigest())
            with tarfile.open(output / manifest['source_archive']) as archive:
                self.assertEqual(archive.extractfile('HeavenBench-1.2.3/tracked.cpp').read(), b'current local edit')
            previous_archive = (output / manifest['source_archive']).read_bytes()
            git('add', '-f', 'sl.interposer.dll')
            rejected = subprocess.run([sys.executable, str(ROOT / 'scripts/export_repository_sources.py'), '--source-root', str(source),
                '--output-dir', str(output), '--platform', 'windows-experimental-dlss'], capture_output=True, text=True, timeout=30)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn('bring-your-own NVIDIA runtime', rejected.stderr)
            self.assertEqual((output / manifest['source_archive']).read_bytes(), previous_archive)


class DistributionBoundaryTest(unittest.TestCase):
    def test_experimental_docker_graph_does_not_build_the_release_artifact(self):
        import re
        stages = {}; current = None
        for line in (ROOT / 'docker/Dockerfile').read_text().splitlines():
            if match := re.match(r'FROM (\S+) AS (\S+)', line):
                parent, current = match.groups(); stages[current] = {parent} if parent in stages else set()
            elif match := re.match(r'COPY --from=(\S+)', line):
                stages[current].add(match[1])
        def ancestors(stage):
            result = set(stages[stage])
            for dependency in stages[stage]: result |= ancestors(dependency)
            return result
        self.assertIn('linux-fsr3-build', ancestors('linux-fsr3-artifacts'))
        self.assertNotIn('linux-build', ancestors('linux-fsr3-artifacts'))
        self.assertNotIn('linux-fsr3-build', ancestors('linux-artifacts'))
        self.assertIn('linux-runtime-base', ancestors('linux-fsr3-artifacts'))
        self.assertIn('linux-runtime-base', ancestors('linux-artifacts'))

    def test_experimental_workflow_has_no_release_publish_trigger_or_permission(self):
        text = (ROOT / '.github/workflows/frame-generation.yml').read_text()
        self.assertIn('branches: [codex/frame-generation]', text)
        self.assertIn('workflow_dispatch:', text)
        self.assertIn('contents: read', text)
        for forbidden in ['pull_request:', 'contents: write', 'gh release', 'softprops/action-gh-release']:
            self.assertNotIn(forbidden, text)


if __name__ == '__main__':
    unittest.main()
