#!/usr/bin/env python3
"""Compilation-database coverage fixtures; no compiler or GPU is needed."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
CHECKER = ROOT / "scripts/clang_tidy_coverage.py"
spec = importlib.util.spec_from_file_location("clang_tidy_coverage", CHECKER)
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class ClangTidyCoverageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heaven-tidy-coverage-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.database = self.root / "compile_commands.json"
        self.cache = self.root / "CMakeCache.txt"
        self.normal = Path("benchmark/src/main.cpp")
        self.optional = sorted(coverage.FSR_SOURCES) + [coverage.FSR_NATIVE_SOURCE]
        self.sources = [self.normal, *self.optional]
        for relative in self.sources:
            source = self.root / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.touch()

    def configure(self, available, fsr="OFF", native="OFF"):
        self.database.write_text(json.dumps([
            {"directory": str(self.root), "file": str(path), "command": f"clang++ -c {path}"}
            for path in available]))
        self.cache.write_text(f"HEAVEN_ENABLE_FSR3:BOOL={fsr}\nHEAVEN_FSR31_NATIVE_TEST:BOOL={native}\n")

    def check(self, sources=None):
        return coverage.check_coverage(self.root, [self.root / p for p in (sources or self.sources)],
                                       self.database, self.cache)

    def test_sdk_off_discloses_exact_unavailable_sources(self):
        self.configure([self.normal])
        result = self.check()
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["selected"], [self.normal.as_posix()])
        self.assertEqual({entry["source"] for entry in result["unavailable"]},
                         {path.as_posix() for path in self.optional})

    def test_missing_ordinary_and_new_optional_sources_fail(self):
        self.configure([self.normal])
        for missing in (Path("benchmark/src/new_renderer.cpp"),
                        Path("benchmark/src/rhi/frame_generation/new_provider.cpp")):
            result = self.check([*self.sources, missing])
            self.assertEqual(len(result["errors"]), 1)
            self.assertIn(missing.as_posix(), result["errors"][0])
        self.configure([])
        self.assertIn(self.normal.as_posix(), self.check()["errors"][0])

    def test_enabled_sdk_sources_are_all_selected(self):
        self.configure(self.sources, fsr="ON", native="ON")
        result = self.check()
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["unavailable"], [])
        self.assertEqual(set(result["selected"]), {source.as_posix() for source in self.sources})

    def test_missing_enabled_provider_fails(self):
        for source in coverage.FSR_SOURCES:
            self.configure([path for path in self.sources if path != source], fsr="ON", native="ON")
            result = self.check()
            self.assertEqual(len(result["errors"]), 1)
            self.assertIn(source.as_posix(), result["errors"][0])

    def test_native_test_option_has_independent_coverage(self):
        available = [source for source in self.sources if source != coverage.FSR_NATIVE_SOURCE]
        self.configure(available, fsr="ON", native="OFF")
        result = self.check()
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["unavailable"], [{"source": coverage.FSR_NATIVE_SOURCE.as_posix(),
                                                 "reason": "HEAVEN_FSR31_NATIVE_TEST is disabled"}])
        self.configure(available, fsr="ON", native="ON")
        self.assertEqual(len(self.check()["errors"]), 1)

    def test_disabled_option_with_stale_database_entry_fails(self):
        self.configure(self.sources)
        self.assertEqual(len(self.check()["errors"]), len(self.optional))

    def test_invalid_database_and_missing_renderer_option_fail(self):
        self.configure([self.normal])
        self.cache.write_text("HEAVEN_ENABLE_SOMETHING_ELSE:BOOL=OFF\n")
        with self.assertRaisesRegex(ValueError, "HEAVEN_ENABLE_FSR3"):
            self.check()
        self.configure([self.normal])
        for invalid in ({}, [{"file": 3}], [{"file": "relative.cpp"}]):
            self.database.write_text(json.dumps(invalid))
            with self.assertRaises(ValueError):
                self.check()

    def test_cli_reports_unavailable_and_fails_missing_source(self):
        self.configure([self.normal])
        command = [sys.executable, str(CHECKER), "--root", str(self.root), "--compile-db", str(self.database),
                   "--cmake-cache", str(self.cache), *[str(self.root / source) for source in self.sources]]
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("coverage unavailable:"), 8)
        self.configure([])
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn(self.normal.as_posix(), result.stderr)


if __name__ == "__main__":
    unittest.main()
