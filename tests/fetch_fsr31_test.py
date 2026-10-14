#!/usr/bin/env python3
"""Exercise the actual pinned SDK fetcher without downloads or GPU dependencies."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


FETCHER = None


class FetchFsr31Test(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heaven fsr cold fetch ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "source with spaces"
        self.archive = self.root / "fixture.tar.gz"
        self.prefix = f"FidelityFX-SDK-{FETCHER.COMMIT}"
        self.files = {
            "LICENSE.txt": b"fixture MIT notice\n",
            "sdk/include/FidelityFX/host/ffx_types.h": b"fixture SDK header\n",
            "sdk/tools/ffx_shader_compiler/libs/SPIRV-Reflect/LICENSE": b"fixture reflection notice\n",
        }
        self.write_archive()

    def write_archive(self, root_name=None, root_is_directory=True, extra=None):
        name = self.prefix if root_name is None else root_name
        with tarfile.open(self.archive, "w:gz") as archive:
            root = tarfile.TarInfo(name)
            root.type = tarfile.DIRTYPE if root_is_directory else tarfile.REGTYPE
            archive.addfile(root)
            for relative, content in {
                **self.files,
                "sdk/bin/vendor.dll": b"excluded runtime binary",
                "sdk/tools/binary_store/compiler.exe": b"excluded compiler binary",
                "samples/example/media.dds": b"excluded sample media",
                **(extra or {}),
            }.items():
                member = tarfile.TarInfo(f"{name}/{relative}")
                member.size = len(content)
                archive.addfile(member, io.BytesIO(content))
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def fetch(self, expected_digest=None):
        # Only the archive pin is substituted for this deliberately tiny fixture.
        # The checked-in argument parsing, extraction, receipt and reuse paths run.
        argv = ["fetch_fsr31.py", "--archive", str(self.archive), "--output", str(self.output)]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(
            FETCHER, "SHA256", self.digest if expected_digest is None else expected_digest
        ), mock.patch.object(FETCHER.urllib.request, "urlretrieve", side_effect=AssertionError("Unexpected download")), contextlib.redirect_stdout(io.StringIO()):
            FETCHER.main()

    def test_cold_fetch_and_reuse(self):
        self.fetch()
        marker = self.output / "heavenbench-source.json"
        receipt = json.loads(marker.read_text())
        expected = {key: hashlib.sha256(value).hexdigest() for key, value in self.files.items()}
        self.assertEqual(receipt["files_sha256"], expected)
        self.assertEqual(receipt["archive_sha256"], self.digest)
        self.assertEqual(receipt["commit"], FETCHER.COMMIT)
        for relative, content in self.files.items():
            self.assertEqual((self.output / relative).read_bytes(), content)
        before = marker.read_bytes()
        self.fetch()
        self.assertEqual(marker.read_bytes(), before)

    def test_windows_and_posix_manifest_keys(self):
        for path_type in (PureWindowsPath, PurePosixPath):
            self.assertEqual(FETCHER.manifest_key(path_type("sdk/include/FidelityFX/host/ffx_types.h")),
                             "sdk/include/FidelityFX/host/ffx_types.h")
            self.assertEqual(FETCHER.manifest_key(path_type("LICENSE.txt")), "LICENSE.txt")

    def test_unknown_archive_root_is_rejected(self):
        self.write_archive(root_name=self.prefix + "-unexpected")
        with self.assertRaisesRegex(SystemExit, "Unexpected SDK archive prefix"):
            self.fetch()
        self.assertFalse((self.output / "heavenbench-source.json").exists())

    def test_exact_root_file_is_not_accepted_as_directory(self):
        self.write_archive(root_is_directory=False)
        with self.assertRaisesRegex(SystemExit, "Unexpected SDK archive prefix"):
            self.fetch()

    def test_archive_traversal_is_rejected(self):
        self.write_archive(extra={"sdk/../../escaped.txt": b"must not escape"})
        with self.assertRaises(tarfile.FilterError):
            self.fetch()
        self.assertFalse((self.root / "escaped.txt").exists())
        self.assertFalse((self.output / "heavenbench-source.json").exists())

    def test_archive_checksum_mismatch_is_rejected_before_extraction(self):
        pinned = self.digest
        self.archive.write_bytes(self.archive.read_bytes() + b"tampered")
        with self.assertRaisesRegex(SystemExit, "FSR SDK archive checksum mismatch"):
            self.fetch(expected_digest=pinned)
        self.assertFalse(self.output.exists())

    def test_modified_reused_source_is_rejected(self):
        self.fetch()
        (self.output / "sdk/include/FidelityFX/host/ffx_types.h").write_bytes(b"modified")
        with self.assertRaisesRegex(SystemExit, "Modified SDK source"):
            self.fetch()

    def test_missing_reused_source_is_rejected(self):
        self.fetch()
        (self.output / "LICENSE.txt").unlink()
        with self.assertRaisesRegex(SystemExit, "Missing or replaced SDK source"):
            self.fetch()

    def test_injected_reused_source_is_rejected(self):
        self.fetch()
        (self.output / "sdk/injected.h").write_bytes(b"unreviewed")
        with self.assertRaisesRegex(SystemExit, "Unexpected files in pinned SDK source directory"):
            self.fetch()

    def test_provenance_marker_tampering_is_rejected(self):
        self.fetch()
        marker = self.output / "heavenbench-source.json"
        receipt = json.loads(marker.read_text())
        receipt["commit"] = "unreviewed"
        marker.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(SystemExit, "different provenance marker"):
            self.fetch()


def main():
    global FETCHER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch-script", type=Path,
                        default=Path(__file__).resolve().parents[1] / "scripts/fetch_fsr31.py")
    arguments, remaining = parser.parse_known_args()
    spec = importlib.util.spec_from_file_location("heaven_fsr_fetcher_under_test", arguments.fetch_script)
    FETCHER = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(FETCHER)
    unittest.main(argv=[sys.argv[0], *remaining])


if __name__ == "__main__":
    main()
