#!/usr/bin/env python3
"""Independent CPU controls for the production presentation-trace analyzer."""
import argparse
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ANALYZER = ROOT / "benchmark/src/rhi/frame_generation/analyze_present_trace.py"
ANALYZER = None


def sample(present_id, kind, source, begin, status="khr_complete", chain=1):
    complete = status == "khr_complete"
    unwaited = status in ("queue_failed", "unsupported")
    return {"chain": chain, "present_id": present_id, "kind": kind, "source_frame": source,
            "submit_begin_ns": begin, "submit_end_ns": begin + 5,
            "khr_signal_lower_ns": None if unwaited else begin + 10,
            "khr_signal_upper_ns": begin + 20 if complete else None,
            "queue_result": -4 if status == "queue_failed" else 0,
            "status": status, "present_mode": 2, "present_mode_label": "fifo"}


def document(samples=None):
    return {"format": "heaven-present-trace", "schema": 1, "available": True,
            "reason": "Independent fixture", "dropped_samples": 0,
            "semantics": "KHR completion does not establish physical display or input latency.",
            "samples": samples if samples is not None else [
                sample(1, "unattributed", 0, 0), sample(2, "unattributed", 0, 100),
                sample(3, "real", 1, 1_000_000), sample(4, "generated", 2, 2_000_000),
                sample(5, "real", 2, 2_100_000), sample(6, "real", 3, 3_000_000, "unresolved_retired")]}


class PresentAnalysisTests(unittest.TestCase):
    def test_runtime_excludes_startup_and_retains_totals(self):
        result = ANALYZER.analyze(document())
        self.assertEqual(result["retained_samples"], 6)
        self.assertEqual(result["kind_counts"], {"unattributed": 2, "real": 3, "generated": 1})
        runtime = result["attributed_runtime"]
        self.assertEqual(runtime["retained_samples"], 4)
        self.assertEqual(runtime["status_counts"], {"khr_complete": 3, "unresolved_retired": 1})
        self.assertEqual(runtime["interval_count"], 2)
        self.assertEqual(runtime["queue_call_intervals_for_completed_ids"]["mean_ms"], 0.55)
        self.assertEqual(result["unattributed"]["retained_samples"], 2)
        self.assertEqual(result["unattributed"]["queue_call_intervals_for_completed_ids"]["mean_ms"], 0.0001)
        self.assertEqual(result["source_pair_coverage"]["immediate_generated_real_pairs"], 1)
        self.assertEqual(result["source_pair_coverage"]["generated_without_retained_immediate_real"], 0)

    def test_independent_transition_bounds_and_chain_kind_subsets(self):
        runtime = ANALYZER.analyze(document())["attributed_runtime"]
        bounds = runtime["khr_signal_interval_bounds"]
        self.assertEqual([(b["lower_ns"], b["upper_ns"]) for b in bounds],
                         [(999_990, 1_000_010), (99_990, 100_010)])
        self.assertEqual([(b["from_kind"], b["to_kind"]) for b in bounds],
                         [("real", "generated"), ("generated", "real")])
        self.assertEqual([b["same_source"] for b in bounds], [False, True])
        self.assertEqual(runtime["transitions"]["generated->real"]["interval_count"], 1)
        self.assertEqual(runtime["transitions"]["generated->real"]["khr_signal_interval_lower_bounds"]["mean_ms"], 0.09999)
        self.assertEqual(runtime["by_kind"]["real"]["retained_samples"], 3)
        self.assertEqual(runtime["by_kind"]["real"]["intervals_spanning_other_or_missing_ids"], 1)
        self.assertEqual(runtime["by_chain"]["1"]["retained_samples"], 4)
        self.assertEqual(runtime["by_chain"]["1"]["transitions"]["generated->real"]["interval_count"], 1)

    def test_no_cross_chain_interval_and_unknown_mode(self):
        rows = [sample(1, "real", 1, 0), sample(2, "real", 2, 100, chain=2),
                sample(3, "real", 3, 200)]
        rows[1].update(present_mode=2**31-1, present_mode_label="unknown")
        result = ANALYZER.analyze(document(rows))
        self.assertEqual(result["interval_count"], 1)
        self.assertEqual(result["khr_signal_interval_bounds"][0]["chain"], 1)
        self.assertFalse(result["khr_signal_interval_bounds"][0]["consecutive_retained_ids"])
        self.assertEqual(result["unknown_present_mode_samples"], 1)

    def test_unresolved_id_validation(self):
        rows = [sample(1, "real", 1, 0), sample(1, "real", 2, 100, "unresolved_retired")]
        with self.assertRaisesRegex(ValueError, "Duplicate/nonmonotonic present ID"):
            ANALYZER.analyze(document(rows))

    def test_duplicate_and_nonmonotonic_ids_in_every_status(self):
        statuses = ("pending", "khr_complete", "unsupported", "queue_failed", "wait_failed",
                    "pending_overflow", "unresolved_timeout", "unresolved_retired")
        for status in statuses:
            for first, second in ((3, 3), (3, 2)):
                with self.subTest(status=status, ids=(first, second)):
                    rows = [sample(first, "real", 1, 0), sample(second, "real", 2, 100, status)]
                    with self.assertRaisesRegex(ValueError, "Duplicate/nonmonotonic present ID"):
                        ANALYZER.analyze(document(rows))
        with self.assertRaisesRegex(ValueError, "Duplicate/nonmonotonic present ID"):
            ANALYZER.analyze(document([sample(2, "real", 1, 0), sample(2, "real", 2, 100, chain=2)]))

    def test_invalid_bounds_in_completed_and_unresolved_records(self):
        for lower, upper in ((None, 20), (-1, 20), (30, 20), (10, 4), (10, None), (10, float("nan"))):
            row = sample(1, "real", 1, 0)
            row.update(khr_signal_lower_ns=lower, khr_signal_upper_ns=upper)
            with self.subTest(lower=lower, upper=upper), self.assertRaises(ValueError):
                ANALYZER.analyze(document([row]))
        for status in ("pending", "wait_failed", "pending_overflow", "unresolved_timeout", "unresolved_retired"):
            for changes in ({"khr_signal_upper_ns": 20}, {"khr_signal_lower_ns": -1}):
                row = sample(1, "real", 1, 0, status);row.update(changes)
                with self.subTest(status=status, changes=changes), self.assertRaises(ValueError):
                    ANALYZER.analyze(document([row]))
        for status in ("unsupported", "queue_failed"):
            row = sample(0, "unattributed", 0, 0, status);row["khr_signal_lower_ns"] = 10
            with self.subTest(status=status), self.assertRaises(ValueError):
                ANALYZER.analyze(document([row]))

    def test_known_source_identity_mismatches(self):
        for kind, source in (("real", 0), ("generated", 0), ("unattributed", 1), ("invented", 1)):
            with self.subTest(kind=kind, source=source), self.assertRaises(ValueError):
                ANALYZER.analyze(document([sample(1, kind, source, 0)]))
        for kind, source in (("real", 8), ("generated", 7), ("unattributed", 0)):
            rows = [sample(1, "generated", 7, 0), sample(2, kind, source, 100)]
            with self.subTest(kind=kind, source=source), self.assertRaisesRegex(ValueError, "Generated/restored-real"):
                ANALYZER.analyze(document(rows))

    def test_missing_or_failed_pair_and_reload_not_invented_as_mismatch(self):
        for rows in ([sample(1, "generated", 7, 0)],
                     [sample(1, "generated", 7, 0), sample(3, "real", 8, 100)],
                     [sample(1, "generated", 7, 0, "queue_failed"), sample(2, "real", 8, 100)]):
            result = ANALYZER.analyze(document(rows))
            self.assertEqual(result["source_pair_coverage"]["generated_without_retained_immediate_real"], 1)
        # source_frame is runtime-local and can reset without a native chain replacement.
        ANALYZER.analyze(document([sample(1, "real", 90, 0), sample(2, "real", 1, 100)]))

    def test_zero_unsupported_ids_and_empty_history_remain_available_as_missing(self):
        rows = [sample(0, "unattributed", 0, 0, "unsupported"),
                sample(0, "unattributed", 0, 100, "unsupported")]
        result = ANALYZER.analyze(document(rows))
        self.assertIsNone(result["completion_bound_widths"])
        self.assertEqual(result["attributed_runtime"]["retained_samples"], 0)
        self.assertEqual(result["khr_signal_interval_bounds"], [])
        self.assertEqual(ANALYZER.analyze(document([]))["retained_samples"], 0)

    def test_schema_identity_types_and_submission_bounds(self):
        bad = document();bad["schema"] = 2
        with self.assertRaises(ValueError): ANALYZER.analyze(bad)
        for key, value in (("present_id", True), ("source_frame", -1), ("chain", 0),
                           ("submit_begin_ns", -1), ("submit_end_ns", -1)):
            row = sample(1, "real", 1, 0);row[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                ANALYZER.analyze(document([row]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--analyzer", type=Path, default=DEFAULT_ANALYZER)
    args, remaining = parser.parse_known_args()
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("heaven_present_analyzer", args.analyzer.resolve())
    ANALYZER = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ANALYZER)
    unittest.main(argv=[sys.argv[0], *remaining])
