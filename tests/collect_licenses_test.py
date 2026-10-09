#!/usr/bin/env python3
"""Check notice provenance, compressed texts, and incomplete release inputs."""

import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/collect_licenses.py"
SPEC = importlib.util.spec_from_file_location("collect_licenses", SCRIPT)
COLLECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COLLECTOR)


class LicenseCollectorTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven-license-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repository = self.root / "source"
        self.output = self.root / "output"
        for name in COLLECTOR.VENDORS:
            self.write(f"lib/{name}/LICENSE", f"{name} license\n")
        self.write("LICENSE", "project license\n")
        self.write("lib/lz4/lz4.c", "/* Copyright LZ4 authors; license text. */\nint code;\n")
        self.write("lib/filament/third_party/mikktspace/src/mikktspace.c",
                   "/** Documentation. */\n/* Copyright Mikk authors; license text. */\n")
        self.write("benchmark/third_party/stb_vorbis.c",
                   "int code;\n/* This software is available under 2 licenses\nnotice */\n")
        self.write("packaging/licenses/component-LICENSE.MIT", "full component license\n")
        self.write("benchmark/assets/sky/NOTICE", "redistributable sky notice\n")
        self.write("lib/filament/tests/LICENSE", "test-only notice\n")
        self.write("lib/filament/build/NOTICE", "generated build notice\n")
        self.addCleanup(patch.stopall)
        patch.object(COLLECTOR, "ROOT", self.repository).start()

    def write(self, relative, content):
        path = self.repository / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_manifest_preserves_provenance_and_complete_texts(self):
        external = self.root / "installed/share/library"
        external.mkdir(parents=True)
        text = b"Full external copyright and license text.\n"
        (external / "copyright.gz").write_bytes(gzip.compress(text))
        count = COLLECTOR.collect(self.output, [f"installed={external.parent}"])
        manifest = json.loads((self.output / "MANIFEST.json").read_text())
        paths = [entry["path"] for entry in manifest["files"]]
        self.assertEqual(len(paths), count)
        self.assertEqual(paths, sorted(set(paths)))
        self.assertIn("repository/benchmark/assets/sky/NOTICE", paths)
        self.assertIn("repository/packaging/licenses/component-LICENSE.MIT", paths)
        self.assertIn("repository/lib/lz4/lz4.c.LICENSE.txt", paths)
        self.assertNotIn("repository/lib/filament/tests/LICENSE", paths)
        self.assertNotIn("repository/lib/filament/build/NOTICE", paths)
        self.assertEqual((self.output / "dependencies/installed/library/copyright").read_bytes(), text)
        for entry in manifest["files"]:
            self.assertEqual(hashlib.sha256((self.output / entry["path"]).read_bytes()).hexdigest(),
                             entry["sha256"])
        embedded = self.output / "repository/lib/filament/third_party/mikktspace/src/mikktspace.c.LICENSE.txt"
        self.assertIn("Copyright Mikk", embedded.read_text())
        self.assertNotIn("Documentation", embedded.read_text())

    def test_missing_vendor_notice_fails(self):
        (self.repository / "lib/bcdec/LICENSE").unlink()
        with self.assertRaisesRegex(ValueError, "No license files found for bcdec"):
            COLLECTOR.collect(self.output, [])

    def test_bundled_opensubdiv_license_and_notice_keep_exact_bytes(self):
        original = SCRIPT.parents[1] / "lib/OpenSubdiv"
        bundled = self.repository / "lib/OpenSubdiv"
        (bundled / "LICENSE").unlink()
        for name in ("LICENSE.txt", "NOTICE.txt"):
            (bundled / name).write_bytes((original / name).read_bytes())
        COLLECTOR.collect(self.output, [])
        manifest = json.loads((self.output / "MANIFEST.json").read_text())["files"]
        hashes = {entry["path"]: entry["sha256"] for entry in manifest}
        for name in ("LICENSE.txt", "NOTICE.txt"):
            relative = "repository/lib/OpenSubdiv/" + name
            content = (original / name).read_bytes()
            self.assertEqual((self.output / relative).read_bytes(), content)
            self.assertEqual(hashes[relative], hashlib.sha256(content).hexdigest())

    def test_external_directory_without_notices_fails(self):
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaisesRegex(ValueError, "No license texts found"):
            COLLECTOR.collect(self.output, [f"empty={empty}"])

    def test_label_cannot_escape_output(self):
        with self.assertRaisesRegex(ValueError, "Invalid or duplicate dependency label"):
            COLLECTOR.collect(self.output, [f"../outside={self.repository}"])

    def test_compressed_destination_collision_fails(self):
        external = self.root / "external"
        external.mkdir()
        (external / "LICENSE").write_bytes(b"first notice")
        (external / "LICENSE.gz").write_bytes(gzip.compress(b"different notice"))
        with self.assertRaisesRegex(ValueError, "Duplicate license destination"):
            COLLECTOR.collect(self.output, [f"external={external}"])


if __name__ == "__main__":
    unittest.main()
