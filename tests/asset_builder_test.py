#!/usr/bin/env python3
"""Test installer acquisition and atomic asset publication without original assets."""

import hashlib
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import unittest
from unittest.mock import patch
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/asset_builder/build_assets.py"
MODULE_SPEC = importlib.util.spec_from_file_location("asset_builder", SCRIPT)
BUILDER = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = BUILDER
MODULE_SPEC.loader.exec_module(BUILDER)

CORE = b"ar03synthetic core archive"
HEAVEN = b"ar03synthetic heaven archive"
MANIFEST = "heavenbench-assets.json"
WORLD = "heaven/heaven/heaven.world"
NORMAL = "heaven/heaven/textures/ruins/ruins_ground_n.dds"
HEIGHT = "heaven/heaven/generated/ground/ruins_ground_h.dds"
FEATURE_ASSETS = (
    "heaven/heaven/meshes/environment/fly_island1.mesh",
    "heaven/heaven/meshes/environment/fly_island2.mesh",
    "heaven/heaven/meshes/environment/fly_island3.mesh",
    "heaven/heaven/textures/environment/grass_d.dds",
    "heaven/heaven/textures/environment/grass_irregularity_d.dds",
    "heaven/heaven/textures/environment/grass_variation_d.dds",
    "heaven/heaven/textures/environment/clouds1_d.dds",
    "heaven/heaven/textures/environment/clouds2_d.dds",
    "heaven/heaven/textures/environment/clouds_mask0_d.dds",
    "heaven/heaven/textures/sfx/fire_glow_dr.dds",
    "heaven/heaven/textures/sfx/spark_d.dds",
)


def asset_entries():
    entries = {name: b"synthetic nonempty asset\n" for name in (
        WORLD, NORMAL, HEIGHT,
        "heaven/sky/moon_disk.png", "heaven/sky/moon_normal.png",
        "heaven/sky/milkyway.png", "heaven/sky/README.md", "heaven/sky/LICENSE.md",
        "heaven/loading/loading_screen.png", "heaven/attribution/DeepBump.md",
        "heaven/attribution/bcdec.md", *FEATURE_ASSETS)}
    manifest = {
        "schema_version": 1, "format": "heavenbench-assets",
        "data_root_prefix": "heaven", "entry_count": len(entries),
        "derived_assets": [{
            "generator_id": "fixture-ground-height", "generator_version": 1,
            "source_path": "heaven/textures/ruins/ruins_ground_n.dds",
            "output_path": "heaven/generated/ground/ruins_ground_h.dds",
            "source_fingerprint_fnv1a64": "1234567890abcdef",
            "output_fingerprint_fnv1a64": "abcdef1234567890",
            "attribution": "attribution/DeepBump.md",
            "codec_attribution": "attribution/bcdec.md",
        }],
    }
    entries[MANIFEST] = json.dumps(manifest).encode()
    return entries


def write_zip(path, entries=None):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in (asset_entries() if entries is None else entries).items():
            archive.writestr(name, data)


def installer_bytes(members=None):
    """The shell prefix must never run; payload extraction uses its byte offset."""
    header = b"#!/bin/sh\necho INSTALLER_SHELL_MUST_NOT_RUN >&2\nexit 73\n"
    payload = io.BytesIO()
    if members is None:
        members = [("./data/core.ung", CORE), ("./data/heaven.ung", HEAVEN),
                   ("./bin/not-selected", b"must not be extracted")]
    with tarfile.open(fileobj=payload, mode="w:gz") as archive:
        for item in members:
            if isinstance(item, tarfile.TarInfo):
                archive.addfile(item)
            else:
                name, data = item
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
    data = header + payload.getvalue()
    spec = BUILDER.InstallerSpec(
        url="https://installer.invalid/Unigine_Heaven-4.0.run", size=len(data),
        sha256=hashlib.sha256(data).hexdigest(), payload_offset=len(header))
    return data, spec


class Response(io.BytesIO):
    def __init__(self, data, content_length=None):
        super().__init__(data)
        self.headers = {"Content-Length": str(len(data) if content_length is None else content_length)}
        self.status = 200

    def geturl(self):
        return "https://installer.invalid/Unigine_Heaven-4.0.run"


class AssetBuilderHelpersTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven builder helpers ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data, self.spec = installer_bytes()
        self.installer = self.root / "Unigine_Heaven-4.0.run"

    def save_installer(self, data=None):
        self.installer.write_bytes(self.data if data is None else data)
        return self.installer

    def test_cached_installer_is_verified_without_network(self):
        self.save_installer()
        with patch.object(BUILDER, "download_installer", side_effect=AssertionError("network used")):
            result = BUILDER.acquire_installer(self.installer, spec=self.spec)
        self.assertEqual(result["bytes"], len(self.data))
        self.assertEqual(result["sha256"], self.spec.sha256)
        self.assertFalse(result["downloaded"])
        self.assertEqual(self.installer.read_bytes(), self.data)

    def test_installer_selection_reuses_local_candidates_in_order(self):
        repo = self.root / "repository"
        cwd = self.root / "working directory"
        (repo / "data").mkdir(parents=True)
        cwd.mkdir()
        name = "Unigine_Heaven-4.0.run"
        candidates = [cwd / name, repo / name, repo / "data" / name]
        self.assertEqual(BUILDER.select_installer(None, repo, cwd), candidates[-1])
        for candidate in reversed(candidates):
            candidate.write_bytes(self.data)
            self.assertEqual(BUILDER.select_installer(None, repo, cwd), candidate)
        explicit = self.root / "explicit installer.run"
        self.assertEqual(BUILDER.select_installer(explicit, repo, cwd), explicit)

    def test_invalid_cache_is_not_replaced_or_redownloaded(self):
        for data in (self.data[:-1], self.data[:-1] + bytes([self.data[-1] ^ 1])):
            with self.subTest(length=len(data)):
                self.save_installer(data)
                with patch.object(BUILDER, "download_installer", side_effect=AssertionError("network used")):
                    with self.assertRaises(BUILDER.BuildError):
                        BUILDER.acquire_installer(self.installer, spec=self.spec)
                self.assertEqual(self.installer.read_bytes(), data)

    def test_no_download_requires_existing_cache(self):
        with patch.object(BUILDER, "download_installer", side_effect=AssertionError("network used")):
            with self.assertRaises(BUILDER.BuildError):
                BUILDER.acquire_installer(self.installer, no_download=True, spec=self.spec)
        self.assertFalse(self.installer.exists())

    def test_download_publishes_only_verified_complete_file(self):
        destination = self.installer

        class ObservedResponse(Response):
            def read(self, size=-1):
                if destination.exists():
                    raise AssertionError("download exposed final file before verification")
                return super().read(size)

        with patch("urllib.request.build_opener") as factory:
            factory.return_value.open.return_value = ObservedResponse(self.data)
            result = BUILDER.download_installer(self.installer, spec=self.spec)
        self.assertEqual(result["sha256"], self.spec.sha256)
        self.assertEqual(self.installer.read_bytes(), self.data)
        self.assertEqual(list(self.root.iterdir()), [self.installer])

    def test_absent_installer_is_acquired_then_reused(self):
        with patch("urllib.request.build_opener") as factory:
            factory.return_value.open.return_value = Response(self.data)
            result = BUILDER.acquire_installer(self.installer, spec=self.spec)
            reused = BUILDER.acquire_installer(self.installer, spec=self.spec)
            self.assertEqual(factory.return_value.open.call_count, 1)
        self.assertTrue(result["downloaded"])
        self.assertFalse(reused["downloaded"])
        self.assertEqual(result["sha256"], reused["sha256"])

    def test_download_size_and_hash_failures_remove_staging_file(self):
        cases = (
            (self.data[:-1], len(self.data)),
            (self.data + b"unexpected trailing bytes", len(self.data)),
            (self.data[:-1] + bytes([self.data[-1] ^ 1]), len(self.data)),
            (self.data, len(self.data) + 1),
        )
        for payload, announced in cases:
            with self.subTest(size=len(payload), announced=announced):
                with patch("urllib.request.build_opener") as factory:
                    factory.return_value.open.return_value = Response(payload, announced)
                    with self.assertRaises(BUILDER.BuildError):
                        BUILDER.download_installer(self.installer, spec=self.spec)
                self.assertEqual(list(self.root.iterdir()), [])

    def test_interrupted_download_removes_partial_bytes(self):
        for error in (OSError("fixture connection interrupted"),
                      http.client.IncompleteRead(b"partial chunk", 123)):
            with self.subTest(error=type(error).__name__):
                class InterruptedResponse(Response):
                    def read(self, size=-1):
                        if self.tell():
                            raise error
                        return super().read(17)

                with patch("urllib.request.build_opener") as factory:
                    factory.return_value.open.return_value = InterruptedResponse(self.data)
                    with self.assertRaises(BUILDER.BuildError):
                        BUILDER.download_installer(self.installer, spec=self.spec)
                self.assertEqual(list(self.root.iterdir()), [])

    def test_https_downgrade_is_rejected_without_publishing(self):
        request = urllib.request.Request(self.spec.url)
        handler = BUILDER.HttpsOnlyRedirectHandler()
        with self.assertRaises(BUILDER.BuildError):
            handler.redirect_request(request, None, 302, "Found", {},
                                     "http://installer.invalid/Unigine_Heaven-4.0.run")

        class DowngradedResponse(Response):
            def geturl(self):
                return "http://installer.invalid/Unigine_Heaven-4.0.run"

        with patch("urllib.request.build_opener") as factory:
            factory.return_value.open.return_value = DowngradedResponse(self.data)
            with self.assertRaises(BUILDER.BuildError):
                BUILDER.download_installer(self.installer, spec=self.spec)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_existing_installer_is_never_replaced_by_downloader(self):
        self.save_installer(b"previous cached installer")
        with patch("urllib.request.build_opener", side_effect=AssertionError("network used")):
            with self.assertRaises(BUILDER.BuildError):
                BUILDER.download_installer(self.installer, spec=self.spec)
        self.assertEqual(self.installer.read_bytes(), b"previous cached installer")
        self.assertEqual(list(self.root.iterdir()), [self.installer])

    def test_selected_archives_are_copied_without_executing_installer(self):
        self.save_installer()
        destination = self.root / "selected archives"
        result = BUILDER.extract_installer_archives(self.installer, destination, spec=self.spec)
        self.assertEqual(set(result), {"core", "heaven"})
        for name, expected in (("core", CORE), ("heaven", HEAVEN)):
            self.assertEqual(Path(result[name]["path"]).read_bytes(), expected)
            self.assertEqual(result[name]["bytes"], len(expected))
            self.assertEqual(result[name]["sha256"], hashlib.sha256(expected).hexdigest())
        self.assertEqual(len(list(destination.rglob("*"))), 2)

    def test_missing_duplicate_and_unsafe_tar_members_are_rejected(self):
        valid = [("./data/core.ung", CORE), ("./data/heaven.ung", HEAVEN)]
        cases = {
            "missing": valid[:1],
            "duplicate": valid + [valid[0]],
            "normalized duplicate": valid + [("data/core.ung", CORE)],
            "traversal": valid + [("./data/../../escaped.ung", b"escape")],
            "absolute": valid + [("/escaped.ung", b"escape")],
            "backslash": valid + [("data\\..\\escaped.ung", b"escape")],
            "empty selected archive": [("./data/core.ung", b""), valid[1]],
        }
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
            member = tarfile.TarInfo("./data/core.ung")
            member.type = kind
            member.linkname = "../../outside"
            cases[f"link {kind!r}"] = [member, valid[1]]
        for label, members in cases.items():
            with self.subTest(case=label):
                data, spec = installer_bytes(members)
                self.save_installer(data)
                destination = self.root / label.replace("/", "_")
                with self.assertRaises(BUILDER.BuildError):
                    BUILDER.extract_installer_archives(self.installer, destination, spec=spec)
                self.assertFalse((self.root / "escaped.ung").exists())

    def test_extraction_refuses_existing_directory(self):
        self.save_installer()
        destination = self.root / "existing"
        destination.mkdir()
        sentinel = destination / "keep.txt"
        sentinel.write_bytes(b"keep")
        with self.assertRaises(BUILDER.BuildError):
            BUILDER.extract_installer_archives(self.installer, destination, spec=self.spec)
        self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_damaged_tar_payload_is_rejected_after_installer_verification(self):
        data = self.data[:self.spec.payload_offset] + b"not a gzip tar stream"
        spec = BUILDER.InstallerSpec(
            self.spec.url, len(data), hashlib.sha256(data).hexdigest(),
            self.spec.payload_offset)
        self.save_installer(data)
        with self.assertRaises(BUILDER.BuildError):
            BUILDER.extract_installer_archives(self.installer, self.root / "bad payload", spec=spec)

    def test_valid_asset_zip_and_corrupt_payload_validation(self):
        archive = self.root / "valid assets.zip"
        write_zip(archive)
        BUILDER.validate_asset_zip(archive)
        data = bytearray(archive.read_bytes())
        marker = b"synthetic nonempty asset\n"
        data[data.index(marker)] ^= 1
        archive.write_bytes(data)
        with self.assertRaises(BUILDER.BuildError):
            BUILDER.validate_asset_zip(archive)

    def test_every_optional_feature_resource_is_required_and_nonempty(self):
        # Keep the manifest count correct, so failure proves feature-resource
        # validation rather than the pre-existing generic ZIP-count check.
        for resource in FEATURE_ASSETS:
            for empty in (False, True):
                with self.subTest(resource=resource, empty=empty):
                    entries = asset_entries()
                    if empty:
                        entries[resource] = b""
                    else:
                        del entries[resource]
                    manifest = json.loads(entries[MANIFEST])
                    manifest["entry_count"] = len(entries) - 1
                    entries[MANIFEST] = json.dumps(manifest).encode()
                    archive = self.root / "missing optional feature.zip"
                    write_zip(archive, entries)
                    with self.assertRaisesRegex(BUILDER.BuildError, re.escape(resource)):
                        BUILDER.validate_asset_zip(archive)

    def test_builder_inventory_matches_renderer_owned_feature_assets(self):
        inventory = (ROOT / "benchmark/src/engine/feature_assets.h").read_text(encoding="utf-8")
        renderer_paths = {"heaven/" + path for path in re.findall(
            r'return \{"(heaven/[^"\n]+)"', inventory)}
        self.assertEqual(renderer_paths, set(FEATURE_ASSETS))
        self.assertEqual(renderer_paths, BUILDER.REQUIRED_FEATURE_ASSETS)

    def test_incomplete_or_unsafe_asset_zip_is_rejected(self):
        cases = []
        for missing in (MANIFEST, WORLD, HEIGHT, "heaven/attribution/bcdec.md"):
            entries = asset_entries()
            del entries[missing]
            cases.append((f"missing {missing}", entries))
        entries = asset_entries()
        entries["../outside"] = b"unsafe"
        cases.append(("traversal", entries))
        entries = asset_entries()
        manifest = json.loads(entries[MANIFEST])
        manifest["schema_version"] = 99
        entries[MANIFEST] = json.dumps(manifest).encode()
        cases.append(("unsupported schema", entries))
        entries = asset_entries()
        entries[MANIFEST] = b"{broken JSON"
        cases.append(("malformed manifest", entries))
        for label, entries in cases:
            with self.subTest(case=label):
                archive = self.root / "invalid.zip"
                write_zip(archive, entries)
                with self.assertRaises(BUILDER.BuildError):
                    BUILDER.validate_asset_zip(archive)

    def test_schema_version_requires_integer_not_boolean_or_float(self):
        for version in (True, 1.0):
            with self.subTest(version=version, type=type(version).__name__):
                entries = asset_entries()
                manifest = json.loads(entries[MANIFEST])
                manifest["schema_version"] = version
                entries[MANIFEST] = json.dumps(manifest).encode()
                archive = self.root / "invalid schema.zip"
                write_zip(archive, entries)
                with self.assertRaises(BUILDER.BuildError):
                    BUILDER.validate_asset_zip(archive)


@unittest.skipIf(os.name == "nt", "executable Python tool shims use POSIX shebangs")
class AssetBuilderCliTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven builder cli ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.installer = self.root / "cached installer with spaces.run"
        data, self.spec = installer_bytes()
        self.installer.write_bytes(data)
        self.output = self.root / "output with spaces" / "assets.zip"
        self.output.parent.mkdir()
        self.work = self.root / "work with spaces"
        self.log = self.root / "tool calls.jsonl"
        self.fixture_zip = self.root / "tool output fixture.zip"
        write_zip(self.fixture_zip)
        self.extractor = self.make_tool("extractor with spaces", """
            import json, os, pathlib, sys
            with open(os.environ['BUILDER_TEST_LOG'], 'a') as log:
                log.write(json.dumps({'tool': 'extractor', 'argv': sys.argv[1:]}) + '\\n')
            assert len(sys.argv) == 4 and sys.argv[1] == '-o', sys.argv
            source, destination = map(pathlib.Path, sys.argv[2:])
            assert source.read_bytes().startswith(b'ar03synthetic ')
            destination.mkdir(parents=True, exist_ok=True)
            target = destination / ('heaven/heaven.world' if source.stem == 'heaven' else 'core/materials/base.mat')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'fixture extracted asset')
            sys.exit(int(os.environ.get('BUILDER_TEST_EXTRACT_STATUS', '0')))
        """)
        self.packer = self.make_tool("packer with spaces", """
            import json, os, pathlib, shutil, sys
            with open(os.environ['BUILDER_TEST_LOG'], 'a') as log:
                log.write(json.dumps({'tool': 'packer', 'argv': sys.argv[1:]}) + '\\n')
            args = sys.argv[1:]
            data = pathlib.Path(args[args.index('--data') + 1])
            output = pathlib.Path(args[args.index('--pack-assets') + 1])
            assert (data / 'heaven/heaven.world').is_file()
            assert (data.parent / 'core/core/materials/base.mat').is_file()
            assert '--clouds' in args
            assert '--pack-move' not in args
            expected_resources = os.environ.get('BUILDER_TEST_RESOURCE_DIR')
            if expected_resources:
                resources = pathlib.Path(os.environ['HEAVEN_RESOURCE_DIR'])
                assert resources.is_absolute()
                assert resources.resolve() == pathlib.Path(expected_resources).resolve()
                assert resources.is_dir()
            existing = os.environ.get('BUILDER_TEST_EXISTING')
            if existing:
                assert pathlib.Path(existing).read_bytes() == b'previous verified assets'
                assert output.resolve() != pathlib.Path(existing).resolve()
                assert output.parent.parent.resolve() == pathlib.Path(existing).parent.resolve()
            raced = os.environ.get('BUILDER_TEST_RACE_OUTPUT')
            if raced:
                pathlib.Path(raced).write_bytes(b'concurrent output must survive')
            if os.environ.get('BUILDER_TEST_NO_OUTPUT') != '1':
                shutil.copyfile(os.environ['BUILDER_TEST_ZIP'], output)
            sys.exit(int(os.environ.get('BUILDER_TEST_PACK_STATUS', '0')))
        """)
        self.runner = self.root / "invoke real CLI.py"
        self.runner.write_text(textwrap.dedent("""
            import importlib.util, json, os, pathlib, sys
            from unittest.mock import patch
            spec = importlib.util.spec_from_file_location('asset_builder_cli_fixture', sys.argv[1])
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            module.OFFICIAL_INSTALLER = module.InstallerSpec(**json.loads(os.environ['BUILDER_TEST_SPEC']))
            with patch.object(module, 'download_installer', side_effect=AssertionError('local reuse attempted network')):
                raise SystemExit(module.main(sys.argv[2:]))
        """), encoding="utf-8")

    def make_tool(self, name, source):
        path = self.root / name
        path.write_text(f"#!{sys.executable}\n" + textwrap.dedent(source), encoding="utf-8")
        path.chmod(0o755)
        return path

    def run_builder(self, *extra, **environment):
        env = dict(os.environ)
        for name in tuple(env):
            if name.startswith("HEAVEN_") or name.startswith("BUILDER_TEST_"):
                del env[name]
        env.update(BUILDER_TEST_LOG=str(self.log), BUILDER_TEST_ZIP=str(self.fixture_zip),
                   BUILDER_TEST_SPEC=json.dumps({
                       "url": self.spec.url, "size": self.spec.size,
                       "sha256": self.spec.sha256, "payload_offset": self.spec.payload_offset}),
                   **environment)
        return subprocess.run([
            sys.executable, str(self.runner), str(SCRIPT),
            "--installer", str(self.installer), "--work-dir", str(self.work),
            "--output", str(self.output), "--extractor", str(self.extractor),
            "--packer", str(self.packer), "--skip-build", "--no-download", *extra,
        ], cwd=self.root, env=env, text=True, capture_output=True, timeout=20, check=False)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def assert_failed_without_publication(self, result, previous=None):
        self.assertGreater(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        if previous is None:
            self.assertFalse(self.output.exists())
        else:
            self.assertEqual(self.output.read_bytes(), previous)
        self.assertEqual(list(self.output.parent.iterdir()),
                         [self.output] if previous is not None else [])
        self.assertEqual(list(self.work.glob("stage-*")), [])
        self.assertEqual(list(self.work.rglob("provenance.json")), [])

    def test_cached_installer_success_with_spaces_and_atomic_output(self):
        original = self.installer.read_bytes()
        result = self.run_builder()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.installer.read_bytes(), original)
        self.assertEqual(self.output.read_bytes(), self.fixture_zip.read_bytes())
        self.assertEqual([entry["tool"] for entry in self.calls()], ["extractor", "extractor", "packer"])
        self.assertNotIn("INSTALLER_SHELL_MUST_NOT_RUN", result.stdout + result.stderr)
        receipts = list(self.work.rglob("provenance.json"))
        self.assertEqual(len(receipts), 1)
        receipt = json.loads(receipts[0].read_text())
        self.assertEqual(receipt["installer"]["sha256"], self.spec.sha256)
        self.assertFalse(receipt["installer"]["downloaded"])
        self.assertEqual(receipt["archives"]["core"]["sha256"], hashlib.sha256(CORE).hexdigest())
        self.assertEqual(receipt["archives"]["heaven"]["sha256"], hashlib.sha256(HEAVEN).hexdigest())
        self.assertEqual(receipt["output"]["path"], str(self.output))
        self.assertEqual(receipt["output"]["sha256"], hashlib.sha256(self.output.read_bytes()).hexdigest())
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])

    def test_stale_packer_missing_cloud_preserves_existing_output(self):
        resource = "heaven/heaven/textures/environment/clouds1_d.dds"
        entries = asset_entries()
        del entries[resource]
        manifest = json.loads(entries[MANIFEST])
        manifest["entry_count"] = len(entries) - 1
        entries[MANIFEST] = json.dumps(manifest).encode()
        write_zip(self.fixture_zip, entries)
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        result = self.run_builder("--overwrite")
        self.assert_failed_without_publication(result, previous)
        self.assertIn(resource, result.stderr)
        self.assertIn("missing or empty runtime resources", result.stderr)
        self.assertEqual(list(self.work.glob("stage-*")), [])

    def test_existing_output_requires_explicit_overwrite(self):
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        result = self.run_builder()
        self.assert_failed_without_publication(result, previous)
        self.assertEqual(self.calls(), [])

    def test_relative_and_empty_resource_overrides_reach_packer_correctly(self):
        relative = Path("relative resource directory")
        (self.root / relative).mkdir()
        for value, expected in ((str(relative), self.root / relative),
                                ("", ROOT / "benchmark/assets")):
            with self.subTest(override=value):
                previous_receipts = set(self.work.rglob("provenance.json"))
                result = self.run_builder(
                    "--overwrite", HEAVEN_RESOURCE_DIR=value,
                    BUILDER_TEST_RESOURCE_DIR=str(expected))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.output.read_bytes(), self.fixture_zip.read_bytes())
                receipts = set(self.work.rglob("provenance.json")) - previous_receipts
                self.assertEqual(len(receipts), 1)
                receipt = json.loads(receipts.pop().read_text())
                self.assertEqual(receipt["resource_directory"], str(expected))

    def test_invalid_installer_preserves_output_before_tools_run(self):
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        self.installer.write_bytes(b"corrupt cached installer")
        result = self.run_builder("--overwrite")
        self.assert_failed_without_publication(result, previous)
        self.assertEqual(self.calls(), [])
        self.assertEqual(self.installer.read_bytes(), b"corrupt cached installer")

    def test_missing_tar_member_preserves_output_before_tools_run(self):
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        data, self.spec = installer_bytes([("./data/core.ung", CORE)])
        self.installer.write_bytes(data)
        result = self.run_builder("--overwrite")
        self.assert_failed_without_publication(result, previous)
        self.assertEqual(self.calls(), [])

    def test_overwrite_keeps_previous_file_until_validated_replacement(self):
        self.output.write_bytes(b"previous verified assets")
        result = self.run_builder("--overwrite", BUILDER_TEST_EXISTING=str(self.output))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output.read_bytes(), self.fixture_zip.read_bytes())

    def test_extractor_failure_preserves_existing_output_and_skips_packer(self):
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        result = self.run_builder("--overwrite", BUILDER_TEST_EXTRACT_STATUS="2")
        self.assert_failed_without_publication(result, previous)
        self.assertEqual([entry["tool"] for entry in self.calls()], ["extractor"])

    def test_packer_nonzero_with_valid_zip_never_publishes(self):
        for status in (2, 5):
            with self.subTest(status=status):
                previous = b"previous verified assets"
                self.output.write_bytes(previous)
                result = self.run_builder("--overwrite", BUILDER_TEST_PACK_STATUS=str(status),
                                          BUILDER_TEST_EXISTING=str(self.output))
                self.assert_failed_without_publication(result, previous)
                self.assertIn(f"failed with exit {status}", result.stderr)

    def test_packer_success_without_output_never_publishes(self):
        result = self.run_builder(BUILDER_TEST_NO_OUTPUT="1")
        self.assert_failed_without_publication(result)

    def test_packer_invalid_zip_preserves_existing_output(self):
        self.fixture_zip.write_bytes(b"not a ZIP archive")
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        result = self.run_builder("--overwrite", BUILDER_TEST_EXISTING=str(self.output))
        self.assert_failed_without_publication(result, previous)

    def test_packer_incomplete_zip_preserves_existing_output(self):
        entries = asset_entries()
        del entries[HEIGHT]
        manifest = json.loads(entries[MANIFEST])
        manifest["entry_count"] -= 1
        entries[MANIFEST] = json.dumps(manifest).encode()
        write_zip(self.fixture_zip, entries)
        previous = b"previous verified assets"
        self.output.write_bytes(previous)
        result = self.run_builder("--overwrite", BUILDER_TEST_EXISTING=str(self.output))
        self.assert_failed_without_publication(result, previous)

    def test_concurrent_output_is_not_replaced_without_overwrite(self):
        result = self.run_builder(BUILDER_TEST_RACE_OUTPUT=str(self.output))
        self.assert_failed_without_publication(result, b"concurrent output must survive")


if __name__ == "__main__":
    unittest.main()
