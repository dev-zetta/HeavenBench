#!/usr/bin/env python3
"""Regression fixtures for the static-analysis gate; no compiler or GPU needed."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parent.parent
WRAPPER = ROOT / "scripts/clang_tidy_gate.py"
spec = importlib.util.spec_from_file_location("clang_tidy_gate", WRAPPER)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def baseline_report():
    diagnostics = []
    for offset, (_, variable, indices) in gate.BASELINE.items():
        diagnostics.append({
            "DiagnosticName": "clang-analyzer-security.ArrayBound",
            "Level": "Error",
            "DiagnosticMessage": {
                "Message": f"Out of bound access to memory after the end of '{variable}'",
                "FilePath": str(ROOT / gate.QUAT_PATH),
                "FileOffset": offset,
                "Replacements": [],
            },
            "Notes": [
                {"FilePath": str(ROOT / gate.HELPER_PATH),
                 "Message": "Calling 'TMat33::packTangentFrame'", "Replacements": []},
                {"FilePath": str(ROOT / gate.QUAT_PATH), "FileOffset": offset,
                 "Message": f"Access of '{variable}' at index {indices[0]}, while it holds only a single 'float' element",
                 "Replacements": []},
            ],
        })
    return {"MainSourceFile": str(ROOT / gate.GRASS_PATH), "Diagnostics": diagnostics}


def texture_usage_reports():
    """Actual reviewed Clang 21.1.8 exports, with portable repository paths."""
    fixtures = json.loads((ROOT / "tests/fixtures/clang_tidy_texture_usage.json").read_text())
    fixtures.append(json.loads((ROOT / "tests/fixtures/clang_tidy/filament_backend_texture_usage.json").read_text()))

    def absolute_paths(value):
        if isinstance(value, dict):
            return {key: str(ROOT / child) if key in ("FilePath", "MainSourceFile")
                    else absolute_paths(child) for key, child in value.items()}
        if isinstance(value, list):
            return [absolute_paths(child) for child in value]
        return value

    return [absolute_paths(report) for report in fixtures]


class ClangTidyGateTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heaven-tidy-fixture-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.fake_tidy = self.directory / "fake-clang-tidy"
        self.fake_tidy.write_text(
            f"#!{sys.executable}\n"
            "import os, pathlib, signal, sys\n"
            "print('fixture diagnostic remains visible', flush=True)\n"
            "print('fixture stderr remains visible', file=sys.stderr, flush=True)\n"
            "report = os.environ.get('FAKE_REPORT')\n"
            "if report:\n"
            "    output = next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('--export-fixes='))\n"
            "    pathlib.Path(output).write_bytes(pathlib.Path(report).read_bytes())\n"
            "status = int(os.environ['FAKE_STATUS'])\n"
            "if status < 0: os.kill(os.getpid(), -status)\n"
            "sys.exit(status)\n", encoding="utf-8")
        self.fake_tidy.chmod(0o755)

    def run_wrapper(self, report=None, status=1, raw=None, extra=()):
        env = dict(os.environ, HEAVEN_CLANG_TIDY_REAL=str(self.fake_tidy),
                   FAKE_STATUS=str(status))
        env.pop("FAKE_REPORT", None)
        if report is not None or raw is not None:
            report_path = self.directory / "export.yaml"
            report_path.write_text(raw if raw is not None else yaml.safe_dump(report),
                                   encoding="utf-8")
            env["FAKE_REPORT"] = str(report_path)
        main_source = report.get("MainSourceFile", str(ROOT / gate.GRASS_PATH)) if isinstance(report, dict) else str(ROOT / gate.GRASS_PATH)
        return subprocess.run([sys.executable, str(WRAPPER), main_source, *extra],
                              env=env, text=True, capture_output=True, check=False)

    def test_reviewed_baseline_preserves_output(self):
        result = self.run_wrapper(baseline_report())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("fixture diagnostic remains visible", result.stdout)
        self.assertIn("fixture stderr remains visible", result.stderr)
        self.assertIn("diagnostics retained above", result.stderr)

    def test_second_reviewed_index(self):
        report = baseline_report()
        report["Diagnostics"][1]["Notes"][1]["Message"] = (
            "Access of 'quat...x' at index 2, while it holds only a single 'float' element")
        self.assertEqual(self.run_wrapper(report).returncode, 0)

    def test_additional_diagnostic_is_failure(self):
        report = baseline_report()
        other = copy.deepcopy(report["Diagnostics"][0])
        other["DiagnosticName"] = "clang-analyzer-core.NullDereference"
        report["Diagnostics"].append(other)
        self.assertEqual(self.run_wrapper(report).returncode, 1)

    def test_changed_diagnostic_fields_are_failures(self):
        mutations = [
            ("DiagnosticName", "clang-analyzer-security.ArrayBoundV2"),
            ("Level", "Warning"),
        ]
        for key, value in mutations:
            with self.subTest(key=key):
                report = baseline_report()
                report["Diagnostics"][0][key] = value
                self.assertEqual(self.run_wrapper(report).returncode, 1)
        mutations = [
            ("Message", "another bounds failure"),
            ("FilePath", str(ROOT / "benchmark/src/rhi/grass_tangent_upload.h")),
            ("FileOffset", 1931),
            ("Replacements", [{"ReplacementText": "fix"}]),
        ]
        for key, value in mutations:
            with self.subTest(key=key):
                report = baseline_report()
                report["Diagnostics"][0]["DiagnosticMessage"][key] = value
                self.assertEqual(self.run_wrapper(report).returncode, 1)

    def test_source_and_trace_changes_are_failures(self):
        report = baseline_report()
        report["MainSourceFile"] = str(ROOT / "benchmark/src/rhi/mesh_store.cpp")
        self.assertEqual(self.run_wrapper(report).returncode, 1)
        for notes in ([], [{"Message": "unrecognized trace", "Replacements": []}]):
            report = baseline_report()
            report["Diagnostics"][0]["Notes"] = notes
            self.assertEqual(self.run_wrapper(report).returncode, 1)
        report = baseline_report()
        report["Diagnostics"][0]["Notes"][1]["Message"] = (
            "Access of 'q...x' at index 4, while it holds only a single 'float' element")
        self.assertEqual(self.run_wrapper(report).returncode, 1)
        report = baseline_report()
        contradictory = copy.deepcopy(report["Diagnostics"][0]["Notes"][1])
        contradictory["Message"] = (
            "Access of 'q...x' at index 4, while it holds only a single 'float' element")
        report["Diagnostics"][0]["Notes"].append(contradictory)
        self.assertEqual(self.run_wrapper(report).returncode, 1)
        with patch.object(gate, "QUAT_SHA256", "0" * 64):
            self.assertFalse(gate.permitted_report(baseline_report()))

    def test_missing_malformed_and_empty_exports_are_failures(self):
        self.assertEqual(self.run_wrapper().returncode, 1)
        for raw in ("", "[invalid yaml", "[]", "null", "Diagnostics: []"):
            with self.subTest(raw=raw):
                self.assertEqual(self.run_wrapper(raw=raw).returncode, 1)

    def test_nonstandard_failures_and_signals_are_preserved(self):
        for status, expected in ((2, 2), (127, 127), (-15, 143)):
            with self.subTest(status=status):
                self.assertEqual(self.run_wrapper(baseline_report(), status=status).returncode, expected)

    def test_success_and_compiler_argument_separator(self):
        self.assertEqual(self.run_wrapper(status=0).returncode, 0)
        self.assertEqual(self.run_wrapper(baseline_report(), extra=("--", "-std=c++23")).returncode, 0)

    def test_caller_cannot_reuse_a_stale_export(self):
        self.assertEqual(self.run_wrapper(baseline_report(), extra=("--export-fixes=stale.yaml",)).returncode, 2)

    def test_four_reviewed_texture_bitmasks_preserve_diagnostics(self):
        reports = texture_usage_reports()
        self.assertEqual({Path(report["MainSourceFile"]).relative_to(ROOT) for report in reports},
                         set(gate.TEXTURE_USAGE_CALLERS))
        for report in reports:
            with self.subTest(caller=report["MainSourceFile"]):
                result = self.run_wrapper(report)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("fixture diagnostic remains visible", result.stdout)
                self.assertIn("fixture stderr remains visible", result.stderr)
                self.assertIn("diagnostics retained above", result.stderr)

    def test_backend_texture_bitmask_requires_exact_reviewed_caller(self):
        caller = Path("benchmark/src/rhi/filament_backend.cpp")
        report = next(report for report in texture_usage_reports()
                      if Path(report["MainSourceFile"]) == ROOT / caller)
        self.assertTrue(gate.permitted_texture_usage_report(report))
        previous_policy = {path: record for path, record in gate.TEXTURE_USAGE_CALLERS.items()
                           if path != caller}
        with patch.object(gate, "TEXTURE_USAGE_CALLERS", previous_policy):
            self.assertFalse(gate.permitted_texture_usage_report(report))

    def test_texture_bitmask_value_cast_and_level_changes_fail(self):
        for baseline in texture_usage_reports():
            mutations = [
                ("Message", "The value '66' provided to the cast expression is not in the valid range of values for 'TextureUsage'"),
                ("Message", "The value '65' provided to the cast expression is not in the valid range of values for 'OtherUsage'"),
                ("FileOffset", 3296),
                ("FilePath", str(ROOT / gate.DRIVER_ENUMS_PATH)),
                ("Replacements", [{"ReplacementText": "fix"}]),
            ]
            for key, value in mutations:
                with self.subTest(caller=baseline["MainSourceFile"], key=key, value=value):
                    report = copy.deepcopy(baseline)
                    report["Diagnostics"][0]["DiagnosticMessage"][key] = value
                    self.assertEqual(self.run_wrapper(report).returncode, 1)
            for key, value in (("DiagnosticName", "clang-analyzer-core.NullDereference"), ("Level", "Warning")):
                report = copy.deepcopy(baseline)
                report["Diagnostics"][0][key] = value
                self.assertEqual(self.run_wrapper(report).returncode, 1)

    def test_texture_bitmask_complete_trace_and_caller_are_pinned(self):
        for baseline in texture_usage_reports():
            for kind in ("note", "offset", "enum", "extra", "missing", "range", "outside", "relative", "unknown-field"):
                with self.subTest(caller=baseline["MainSourceFile"], kind=kind):
                    report = copy.deepcopy(baseline)
                    notes = report["Diagnostics"][0]["Notes"]
                    call = next(note for note in notes if note["Message"].startswith("Calling 'operator|"))
                    if kind == "note": call["Message"] = "Calling an unreviewed operator"
                    elif kind == "offset": call["FileOffset"] += 1
                    elif kind == "enum": notes[0]["FileOffset"] += 1
                    elif kind == "extra": notes.append(copy.deepcopy(notes[-1]))
                    elif kind == "missing": notes.pop()
                    elif kind == "range": call["Ranges"][0]["Length"] += 1
                    elif kind == "outside": call["FilePath"] = "/tmp/unreviewed.cpp"
                    elif kind == "relative": call["FilePath"] = "benchmark/src/rhi/height_sample_batch.cpp"
                    else: call["BuildDirectory"] = "/tmp/injected-trace-field"
                    self.assertEqual(self.run_wrapper(report).returncode, 1)
            report = copy.deepcopy(baseline)
            report["MainSourceFile"] = str(ROOT / "benchmark/src/rhi/mesh_store.cpp")
            self.assertEqual(self.run_wrapper(report).returncode, 1)
            report = copy.deepcopy(baseline)
            report["Diagnostics"].append(copy.deepcopy(report["Diagnostics"][0]))
            self.assertEqual(self.run_wrapper(report).returncode, 1)

    def test_texture_bitmask_source_changes_fail(self):
        for report in texture_usage_reports():
            caller = Path(report["MainSourceFile"]).relative_to(ROOT)
            changed = dict(gate.TEXTURE_USAGE_CALLERS[caller], source_sha256="0" * 64)
            with patch.dict(gate.TEXTURE_USAGE_CALLERS, {caller: changed}):
                self.assertFalse(gate.permitted_texture_usage_report(report))
            for path in gate.TEXTURE_USAGE_SOURCES:
                with self.subTest(caller=caller, dependency=path):
                    with patch.dict(gate.TEXTURE_USAGE_SOURCES, {path: "0" * 64}):
                        self.assertFalse(gate.permitted_texture_usage_report(report))

    def test_texture_bitmask_previous_vendor_offsets_are_not_accepted(self):
        # The NativeCommand include moved only the enum declaration. Keep the
        # complete current trace pinned instead of ignoring dependency offsets.
        for report in texture_usage_reports():
            def previous_offsets(value):
                if isinstance(value, dict):
                    if (value.get("FilePath") == str(ROOT / gate.DRIVER_ENUMS_PATH)
                            and "FileOffset" in value):
                        value["FileOffset"] -= 35
                    for child in value.values():
                        previous_offsets(child)
                elif isinstance(value, list):
                    for child in value:
                        previous_offsets(child)
            previous_offsets(report)
            self.assertEqual(self.run_wrapper(report).returncode, 1)

    def test_texture_bitmask_previous_vendor_source_is_not_accepted(self):
        previous = "5d51ce7e896c626644c1d7765c45388e35229d7f977938a2844eeccda74fdd4c"
        for report in texture_usage_reports():
            with patch.dict(gate.TEXTURE_USAGE_SOURCES, {gate.DRIVER_ENUMS_PATH: previous}):
                self.assertFalse(gate.permitted_texture_usage_report(report))

    def test_texture_bitmask_compile_directory_and_path_aliases(self):
        # Dependency paths emitted as benchmark/../lib identify the same source.
        for report in texture_usage_reports():
            def alias_paths(value):
                if isinstance(value, dict):
                    for key, child in value.items():
                        if key == "FilePath":
                            value[key] = child.replace(str(ROOT / "lib") + "/", str(ROOT / "benchmark/../lib") + "/")
                        else: alias_paths(child)
                elif isinstance(value, list):
                    for child in value: alias_paths(child)
            alias_paths(report)
            report["Diagnostics"][0]["BuildDirectory"] = "/another/ci/build"
            self.assertEqual(self.run_wrapper(report).returncode, 0)

    def test_texture_bitmask_relocated_sources_and_actual_tampering(self):
        checkout = self.directory / "checkout"
        sources = set(gate.TEXTURE_USAGE_SOURCES) | set(gate.TEXTURE_USAGE_CALLERS)
        for path in sources:
            target = checkout / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / path).read_bytes())

        def relocate(value):
            if isinstance(value, dict):
                return {key: str(checkout / Path(child).relative_to(ROOT))
                        if key in ("FilePath", "MainSourceFile") else relocate(child)
                        for key, child in value.items()}
            if isinstance(value, list):
                return [relocate(child) for child in value]
            return value

        for baseline in texture_usage_reports():
            report = relocate(baseline)
            caller = Path(baseline["MainSourceFile"]).relative_to(ROOT)
            with patch.object(gate, "REPO_ROOT", checkout):
                self.assertTrue(gate.permitted_texture_usage_report(report))
                for path in set(gate.TEXTURE_USAGE_SOURCES) | {caller}:
                    with self.subTest(caller=caller, changed_source=path):
                        target = checkout / path
                        original = target.read_bytes()
                        target.write_bytes(original + b"\n// Unreviewed source change\n")
                        try:
                            self.assertFalse(gate.permitted_texture_usage_report(report))
                        finally:
                            target.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
