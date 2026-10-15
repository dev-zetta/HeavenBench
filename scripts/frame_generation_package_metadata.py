#!/usr/bin/env python3
"""Write verified metadata for explicitly experimental SDK-enabled packages."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FG = ROOT / 'benchmark/src/rhi/frame_generation'
FALSE_VALUES = {'', '0', 'OFF', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND'}


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def cache_values(path):
    return {match[1]: match[2] for line in Path(path).read_text().splitlines()
            if (match := re.fullmatch(r'([^/#][^:]*):[^=]+=(.*)', line))}


def enabled(value):
    return value.upper() not in FALSE_VALUES and not value.upper().endswith('-NOTFOUND')


def verify(command):
    result = subprocess.run(command, text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise ValueError(f'SDK provenance verification failed: {result.stdout}{result.stderr}')


def write_metadata(build, stage, binary, licenses, source_manifest, providers, platform):
    build, stage, binary, licenses = map(Path, (build, stage, binary, licenses))
    cache = cache_values(build / 'CMakeCache.txt')
    expected = {'fsr3': (True, False), 'dlss': (False, True), 'both': (True, True)}[providers]
    actual = tuple(enabled(cache.get(key, 'OFF')) for key in ('HEAVEN_ENABLE_FSR3', 'HEAVEN_ENABLE_DLSS'))
    if actual != expected or (platform == 'linux' and providers != 'fsr3'):
        raise ValueError('Experimental provider label does not match the configured SDK build')
    if cache.get('CMAKE_BUILD_TYPE') != 'Release' or cache.get('HEAVEN_CPU_ARCH') != 'portable':
        raise ValueError('Experimental distribution requires a portable Release build')
    manifest = json.loads(Path(source_manifest).read_text(encoding='utf-8-sig'))
    source_archive = Path(source_manifest).parent / manifest['source_archive']
    if Path(manifest['source_archive']).name != manifest['source_archive'] or sha(source_archive) != manifest['source_sha256']:
        raise ValueError('Exact source archive checksum does not match its manifest')
    if Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != (ROOT / 'benchmark').resolve():
        raise ValueError('Build tree does not refer to this source checkout')
    if 'benchmark/CMakeLists.txt' not in manifest.get('files', {}):
        raise ValueError('Source manifest omits the renderer build definition')
    for relative, expected_file in manifest['files'].items():
        path = ROOT / relative
        if re.match(r'^(sl\..*|nvngx.*)\.dll$', path.name, re.I):
            raise ValueError('Exact source archive must not distribute bring-your-own NVIDIA runtime plugins')
        if not path.resolve().is_relative_to(ROOT.resolve()):
            raise ValueError('Source manifest path escapes the checkout: ' + relative)
        actual_file = {'symlink': str(path.readlink())} if path.is_symlink() else sha(path)
        if actual_file != expected_file:
            raise ValueError('Source changed after exact source export: ' + relative)
    for path in stage.rglob('*'):
        if path.is_file() and re.match(r'^(sl\..*|nvngx.*)\.dll$', path.name, re.I):
            raise ValueError('Signed NVIDIA runtime plugins are bring-your-own and must not be packaged: ' + path.name)
    sdk = {}
    if expected[0]:
        source = Path(cache['HEAVEN_FSR31_SOURCE'])
        lock = FG / 'fsr31-source.sha256'
        verify([sys.executable, str(FG / 'verify_fsr31_source.py'), '--sdk', str(source), '--lock', str(lock)])
        for relative, packaged in [('LICENSE.txt', 'FidelityFX-SDK.txt'),
                ('sdk/tools/ffx_shader_compiler/libs/SPIRV-Reflect/LICENSE', 'SPIRV-Reflect.txt')]:
            notice = licenses / 'repository/packaging/licenses' / packaged
            if sha(source / relative) != sha(notice):
                raise ValueError('Packaged FSR notice differs from the verified source: ' + packaged)
        receipt_path = build / 'src/rhi/frame_generation/fsr31_shader_blobs.json'
        receipt = json.loads(receipt_path.read_text())
        if receipt.get('spirv_validated') is not True or len(receipt.get('shaders', [])) != 102:
            raise ValueError('All 102 FSR shader permutations must pass SPIR-V validation')
        sdk['fsr3'] = {'source_lock_sha256': sha(lock), 'shader_receipt_sha256': sha(receipt_path),
                       'shader_permutations': 102, 'spirv_validated': True}
    if expected[1]:
        headers = Path(cache['HEAVEN_STREAMLINE_HEADERS'])
        verify([sys.executable, str(FG / 'dlss_fetch_sdk.py'), '--output', str(headers), '--verify-only'])
        if sha(headers / 'license.txt') != sha(licenses / 'dependencies/Streamline/license.txt'):
            raise ValueError('Packaged Streamline notice differs from verified headers')
        sdk['dlss'] = {'headers_provenance': json.loads((headers / 'heavenbench-source.json').read_text()),
                       'runtime_plugins': 'not included; bring your own official signed production plugins'}
    tag = f"experimental-{providers}-{manifest['source_sha256'][:12]}"
    payload = {'schema_version': 1, 'artifact_tag': tag, 'experimental': True, 'platform': platform,
               'providers_built': providers, 'default_frame_generation': 'off',
               'cmake_cache_sha256': sha(build / 'CMakeCache.txt'), 'binary_sha256': sha(binary),
               'source_archive': source_archive.name, 'source_archive_sha256': manifest['source_sha256'],
               'sdk': sdk, 'acceptance_scope': 'Build, host contracts and package relocation only; native GPU/runtime, visual quality, display cadence and latency are not qualified by packaging.'}
    (stage / 'experimental-frame-generation.json').write_text(json.dumps(payload, indent=2) + '\n')
    shutil.copyfile(source_manifest, stage / 'experimental-source-manifest.json')
    (stage / 'EXPERIMENTAL-FRAME-GENERATION.txt').write_text(
        'HeavenBench experimental frame-generation build\n\n'
        f'Built providers: {providers}. Frame generation defaults off. Select a built provider explicitly in Settings or with --frame-generation. Every tour requesting generation is diagnostic and unscored. Native submissions do not establish display FPS.\n\n'
        'DLSS-enabled Windows variants compile the adapter against verified SDK headers. NVIDIA/Streamline runtime DLLs are not included or extracted by this package pipeline; Streamline SDK inputs extract headers and the license only. Supply the complete official signed production plugin set separately using HEAVEN_DLSS_PLUGIN_DIR and your registered application identifier using HEAVEN_DLSS_PROJECT_ID. Supported Windows/NVIDIA hardware and the official runtime are required; unavailable reasons remain visible.\n\n'
        'This package is experimental. Packaging checks do not qualify native GPU behavior, interpolation quality, display cadence or latency. Original Heaven assets remain external. Matching source archive and checksums accompany the artifact.\n')
    return tag


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'stage', 'binary', 'licenses', 'source-manifest'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--providers', choices=('fsr3', 'dlss', 'both'), required=True)
    parser.add_argument('--platform', choices=('linux', 'windows'), required=True)
    args = parser.parse_args()
    try:
        print(write_metadata(args.build_dir, args.stage, args.binary, args.licenses,
                             args.source_manifest, args.providers, args.platform))
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'Experimental package metadata: {error}\n')


if __name__ == '__main__':
    main()
