#!/usr/bin/env python3
"""Verify source-safe private cache reuse without Docker or C++ compilation."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_docker_cache",
                                             ROOT / "scripts/prepare_docker_cache.py")
CACHE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CACHE)


class DockerCacheTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heaven-cache-fixture-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.input = self.source / "input.txt"
        self.write_source("snapshot A\n")
        self.toolchain = {"packages": "fixture packages", "clang": "fixture clang1"}

    def write_source(self, contents):
        self.input.write_text(contents)
        # Source archive exports deliberately normalize mtimes to the epoch.
        os.utime(self.input, (0, 0))

    def prepare(self, **overrides):
        options = dict(platform="linux", target="portable", toolchain=self.toolchain)
        options.update(overrides)
        return CACHE.prepare_cache(self.source, self.cache, **options)

    @staticmethod
    def put(path, contents="keep"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)

    def test_missing_marker_clears_only_known_compilation_trees(self):
        for name in CACHE.COMPILATION_TREES:
            self.put(self.cache / name / "old-output")
        preserved = (
            "android-downloads/SDL.tar.gz", "dependencies/gradle/bin/gradle",
            "dependencies/SDL2/source.c", "android-gradle-cache/modules/module.jar",
            "android-user/debug.keystore", "packaging-tools/appimagetool",
            "unrelated user directory/file", "root-file",
        )
        for name in preserved:
            self.put(self.cache / name)
        result = self.prepare()
        self.assertTrue(result["invalidated"])
        self.assertEqual(set(result["removed"]), set(CACHE.COMPILATION_TREES))
        for name in CACHE.COMPILATION_TREES:
            self.assertFalse((self.cache / name).exists())
        for name in preserved:
            self.assertEqual((self.cache / name).read_text(), "keep")
        self.assertEqual(json.loads((self.cache / CACHE.MARKER).read_text()), result["state"])

    def test_unchanged_source_and_toolchain_reuse_partial_build(self):
        first = self.prepare()
        output = self.cache / "renderer/partial-object"
        self.put(output)
        second = self.prepare()
        self.assertFalse(second["invalidated"])
        self.assertEqual(first["state"], second["state"])
        self.assertEqual(output.read_text(), "keep")

    def test_reverting_source_after_failed_build_clears_partial_outputs(self):
        original = self.prepare()["state"]["fingerprint"]
        self.write_source("snapshot B\n")
        changed = self.prepare()["state"]["fingerprint"]
        self.assertNotEqual(original, changed)
        # Simulate failure after writing a few objects. The marker already names B.
        output = self.cache / "renderer/partial-object"
        self.put(output, "snapshot B object")
        self.assertEqual(json.loads((self.cache / CACHE.MARKER).read_text())["fingerprint"], changed)
        self.write_source("snapshot A\n")
        result = self.prepare()
        self.assertTrue(result["invalidated"])
        self.assertEqual(result["state"]["fingerprint"], original)
        self.assertFalse(output.exists())

    def test_toolchain_and_target_changes_invalidate(self):
        self.prepare()
        for options in ({"toolchain": {**self.toolchain, "clang": "fixture clang2"}},
                        {"target": "zen1"}, {"platform": "android", "target": "arm64-v8a"}):
            with self.subTest(options=options):
                output = self.cache / "renderer/object"
                self.put(output)
                self.assertTrue(self.prepare(**options)["invalidated"])
                self.assertFalse(output.exists())

    def test_file_addition_deletion_and_mode_are_fingerprinted(self):
        initial = CACHE.source_fingerprint(self.source)
        additional = self.source / "added.h"
        additional.write_text("same\n")
        self.assertNotEqual(CACHE.source_fingerprint(self.source), initial)
        additional.unlink()
        self.assertEqual(CACHE.source_fingerprint(self.source), initial)
        if os.name != "nt":
            self.input.chmod(0o755)
            self.assertNotEqual(CACHE.source_fingerprint(self.source), initial)

    def test_invalid_marker_is_not_trusted(self):
        self.put(self.cache / CACHE.MARKER, "broken JSON")
        output = self.cache / "android-package/stale-resource"
        self.put(output)
        self.assertTrue(self.prepare()["invalidated"])
        self.assertFalse(output.exists())

    def symlink(self, path, target, directory=False):
        try:
            path.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Symlink creation unavailable: {error}")

    def test_compilation_symlink_does_not_delete_target(self):
        outside = self.root / "valuable"
        self.put(outside / "file")
        self.symlink(self.cache / "renderer", outside, directory=True)
        self.prepare()
        self.assertFalse((self.cache / "renderer").is_symlink())
        self.assertEqual((outside / "file").read_text(), "keep")

    def test_marker_symlink_is_rejected_without_deleting_outputs(self):
        outside = self.root / "valuable-marker"
        self.put(outside)
        self.symlink(self.cache / CACHE.MARKER, outside)
        output = self.cache / "renderer/object"
        self.put(output)
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(output.read_text(), "keep")
        self.assertEqual(outside.read_text(), "keep")

    def test_source_escape_is_rejected_before_invalidation(self):
        outside = self.root / "valuable"
        self.put(outside)
        self.symlink(self.source / "escape", outside)
        output = self.cache / "renderer/object"
        self.put(output)
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertEqual(output.read_text(), "keep")

    def test_source_cache_overlap_is_rejected(self):
        for cache in (self.source, self.source / "build", self.root):
            with self.subTest(cache=cache), self.assertRaises(ValueError):
                CACHE.prepare_cache(self.source, cache, platform="linux", target="portable",
                                    toolchain=self.toolchain)
        self.assertEqual(self.input.read_text(), "snapshot A\n")

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is required for stale-mtime reproduction")
    def test_real_ninja_rebuilds_changed_epoch_timestamp_source(self):
        self.prepare()
        build = self.cache / "renderer"
        copy = self.root / "copy_fixture.py"
        copy.write_text("from pathlib import Path\n"
                        "Path('output.txt').write_bytes(Path('../../source/input.txt').read_bytes())\n")

        def configure():
            build.mkdir(parents=True, exist_ok=True)
            python = Path(sys.executable).as_posix().replace("$", "$$")
            (build / "build.ninja").write_text(
                'rule copy\n  command = "' + python + '" "../../copy_fixture.py"\n'
                'build output.txt: copy ../../source/input.txt\n')

        def run():
            subprocess.run(["ninja", "-C", str(build)], check=True, capture_output=True, timeout=30)
            return (build / "output.txt").read_text()

        configure()
        self.assertEqual(run(), "snapshot A\n")
        self.write_source("snapshot B\n")
        self.assertEqual(run(), "snapshot A\n", "Fixture must reproduce Ninja's timestamp hazard")
        self.assertTrue(self.prepare()["invalidated"])
        configure()
        self.assertEqual(run(), "snapshot B\n")


if __name__ == "__main__":
    unittest.main()
