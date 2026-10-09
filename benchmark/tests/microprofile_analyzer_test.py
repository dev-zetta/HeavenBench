#!/usr/bin/env python3
"""Known-interval oracle for the standalone microprofile analyzer."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("analyzer", Path(__file__).resolve().parents[2] / "scripts/analyze_microprofile.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


def fixture():
    context = {k: "0" for k in ("attempt_id", "submission_id", "filament_frame_id", "renderer_id", "view_id", "capture_id")}
    def span(ident, parent, begin, end, thread="1", incomplete=False):
        return dict(id=str(ident), parent_id=str(parent), thread_id=thread, begin_ns=begin, end_ns=end,
                    name=str(ident), category="CPU", context=context, incomplete=incomplete)
    return dict(format="heaven-microprofile", schema=1, session_id="1", origin_ns="12345678901234567", begin_ns=0, end_ns=100,
                clock=dict(unit="nanoseconds", cpu="steady-clock-relative", gpu="frame-relative-uncalibrated"),
                active=False, metadata={"cpu": "Fixture CPU", "gpu": "Fixture GPU", "driver": "Fixture driver",
                    "driver_version": "1.2.3", "api": "Vulkan", "resolution": "1920x1080",
                    "effective_settings": '{"preset":"fixture"}'}, dropped_events=0, overwritten_events=0,
                truncated_labels=0, memory_bytes=4096, threads=[dict(id="1", name="Main"), dict(id="2", name="Worker")],
                spans=[span(1, 0, 0, 100), span(2, 1, 10, 50), span(3, 1, 30, 70), span(4, 2, 20, 25),
                       span(5, 1, 0, 100, "2"), span(6, 0, 0, 100, incomplete=True)],
                frames=[], counters=[], flows=[], gpu_samples=[dict(context=context, pass_id="1", parent_pass_id="0", begin_ns=0,
                    end_ns=900, name="GPU", status="Available", source="HardwareTimestamp", capability="fixture", command_count="10", primitive_count="20", visible_renderables="3"),
                    dict(context=context, pass_id="2", parent_pass_id="0", begin_ns=0, end_ns=0,
                         name="GPU", status="Disjoint", source="HardwareTimestamp", capability="fixture", command_count="0", primitive_count="0", visible_renderables="0")])


class TestAnalyzer(unittest.TestCase):
    def test_interval_union_and_clock_separation(self):
        result = analyzer.analyze(fixture())
        rows = {r["name"]: r for r in result["cpu"]}
        self.assertEqual(rows["1"]["exclusive_ns"], 40)  # [10,70] union, not 80 ns sum
        self.assertEqual(rows["2"]["exclusive_ns"], 35)
        self.assertEqual(rows["1"]["inclusive_distribution_ns"]["p99"], 100)
        self.assertEqual(rows["2"]["exclusive_distribution_ns"]["p95"], 35)
        self.assertEqual(analyzer.distribution([10, 20, 30, 40])["p50"], 25)
        self.assertEqual(analyzer.distribution([10, 20, 30, 40])["p95"], 38.5)
        self.assertEqual(rows["6"]["count"], 0)
        self.assertEqual(rows["6"]["incomplete_count"], 1)
        self.assertEqual(result["gpu"][0]["inclusive_ns"], 900)
        self.assertNotIn("total_ns", result)
        self.assertEqual(result["gpu_statuses"]["Disjoint"], 1)

    def test_loss_and_incomplete_children_withhold_exclusive(self):
        for edit in (lambda d: d.update(dropped_events=1), lambda d: d.update(overwritten_events=1),
                     lambda d: d["spans"][1].update(incomplete=True),
                     lambda d: d["spans"][1].update(end_ns=110)):
            data = fixture()
            edit(data)
            rows = {r["name"]: r for r in analyzer.analyze(data)["cpu"]}
            self.assertIsNone(rows["1"]["exclusive_ns"])
            self.assertEqual(rows["1"]["inclusive_ns"], 100)
            changes = analyzer.compare(data, data)["cpu_mean_per_complete_scope"]
            self.assertNotIn("1", [r["name"] for r in changes])

    def test_thread_activity_unions_nested_overlap_and_partial_history(self):
        data = fixture()
        data["spans"] = data["spans"][:5]
        # Main observed intervals are [10,70] and [80,90]. Nested and
        # overlapping children do not multiply coverage; a parallel worker
        # covers the entire window on its own clock row.
        data["spans"][0].update(begin_ns=80, end_ns=90)
        data["threads"].append(dict(id="3", name="Dormant"))
        result = analyzer.analyze(data)
        rows = {r["thread_id"]: r for r in result["thread_activity"]}
        self.assertEqual(rows["1"]["observed_active_ns"], 70)
        self.assertEqual(rows["1"]["observed_wall_fraction"], .7)
        self.assertEqual(rows["2"]["observed_active_ns"], 100)
        self.assertEqual(rows["3"]["observed_active_ns"], 0)  # no observation, not proven idle
        self.assertFalse(rows["1"]["coverage_incomplete"])
        self.assertNotIn("cpu_utilization", rows["1"])
        self.assertEqual(result["gpu"][0]["inclusive_ns"], 900)  # never part of CPU coverage
        self.assertEqual(analyzer.compare(data, data)["thread_activity_a"], result["thread_activity"])
        for loss in ("dropped_events", "overwritten_events"):
            lost = copy.deepcopy(data)
            lost[loss] = 1
            rows = analyzer.analyze(lost)["thread_activity"]
            self.assertTrue(all(r["coverage_incomplete"] for r in rows))
            self.assertEqual(rows[0]["observed_active_ns"], 70)  # never extrapolate missing work
        data["spans"][0].update(begin_ns=0, end_ns=110, incomplete=True)
        row = analyzer.analyze(data)["thread_activity"][0]
        self.assertEqual(row["observed_active_ns"], 100)
        self.assertEqual(row["incomplete_span_count"], 1)
        self.assertEqual(row["window_clipped_span_count"], 1)
        self.assertTrue(row["coverage_incomplete"])
        data.update(begin_ns=100, end_ns=100)
        row = analyzer.analyze(data)["thread_activity"][0]
        self.assertEqual(row["observed_active_ns"], 0)
        self.assertIsNone(row["observed_wall_fraction"])

    def test_worst_frames_preserve_identity_and_exclude_partial_or_unaccepted(self):
        data = fixture()
        ctx = data["spans"][0]["context"]
        def frame(ident, begin, end, outcome="Accepted", kind="Render", incomplete=False):
            return dict(context={**ctx, "attempt_id": str(ident), "submission_id": str(ident)},
                        begin_ns=begin, end_ns=end, kind=kind, outcome=outcome, incomplete=incomplete)
        data["frames"] = [frame(1, 0, 30), frame(2, 35, 55), frame(3, 56, 96),
                          frame(4, 0, 100, "Rejected"), frame(5, 0, 100, incomplete=True),
                          frame(6, 0, 100, kind="Loading"), frame(7, 0, 100, "Pending"),
                          frame(8, 0, 101), frame(9, 0, 100, "Cancelled")]
        result = analyzer.analyze(data)
        rows = result["worst_frames"]
        self.assertEqual([r["context"]["attempt_id"] for r in rows], ["3", "1", "2"])
        self.assertEqual([r["duration_ns"] for r in rows], [40, 30, 20])
        self.assertEqual(result["accepted_complete_cpu_frame_duration_ns"]["count"], 3)
        self.assertEqual(analyzer.compare(data, data)["worst_frames_a"], rows)
        data["dropped_events"] = 17
        self.assertEqual(analyzer.analyze(data)["worst_frames"], rows)  # observed ranking only
        data["frames"] = [frame(i, 0, i) for i in range(1, 31)]
        rows = analyzer.analyze(data)["worst_frames"]
        self.assertEqual(len(rows), 20)
        self.assertEqual([r["duration_ns"] for r in rows], list(range(30, 10, -1)))
        data["frames"] = []
        self.assertEqual(analyzer.analyze(data)["worst_frames"], [])
        self.assertIsNone(analyzer.analyze(data)["accepted_complete_cpu_frame_duration_ns"])

    def test_metadata_mismatch_refused(self):
        for key in fixture()["metadata"]:
            with self.subTest(key=key):
                other = fixture()
                other["metadata"][key] += " changed"
                with self.assertRaisesRegex(ValueError, "identical nonempty recorded metadata"):
                    analyzer.compare(fixture(), other)
        self.assertTrue(analyzer.compare(fixture(), fixture())["cpu_mean_per_complete_scope"])
        mixed = fixture()
        mixed["metadata"]["settings_mixed"] = "true"
        with self.assertRaisesRegex(ValueError, "mixed settings"):
            analyzer.compare(mixed, mixed)

    def test_incomplete_identity_metadata_refused_only_for_comparison(self):
        for side in ("a", "b"):
            for key in fixture()["metadata"]:
                for value in (None, "", " \t", "UNKNOWN", "(unknown)", "Unknown GPU", "unavailable",
                              "Unavailable (not queried)", "unsupported", "none", "n/a", "not reported"):
                    with self.subTest(side=side, key=key, value=value):
                        incomplete = fixture()
                        if value is None:
                            del incomplete["metadata"][key]
                        else:
                            incomplete["metadata"][key] = value
                        # Old/lightweight or partially reported traces retain
                        # their individual timing and loss analysis unchanged.
                        self.assertTrue(analyzer.analyze(incomplete)["cpu"])
                        left, right = (incomplete, fixture()) if side == "a" else (fixture(), incomplete)
                        with self.assertRaisesRegex(ValueError, rf"missing/unknown fields:.*{side}\.{key}"):
                            analyzer.compare(left, right)
        lightweight = fixture()
        lightweight["metadata"] = {"device": "fixture", "api": "none"}
        self.assertTrue(analyzer.analyze(lightweight)["cpu"])
        with self.assertRaisesRegex(ValueError, r"a\.cpu.*a\.gpu.*a\.driver.*a\.api.*b\.cpu"):
            analyzer.compare(lightweight, lightweight)

    def test_workload_observations_and_comparison_counts(self):
        data = fixture()
        ctx = data["spans"][0]["context"]
        data["counters"] = [dict(thread_id="1", timestamp_ns=t, name="draws", category="Workload", context=ctx, value=v)
                            for t, v in ((30, 7), (10, 3), (20, 5))]
        data["counters"].append(dict(thread_id="2", timestamp_ns=40, name="draws", category="Workload", context=ctx, value=99))
        data["gpu_samples"][0]["draw_instance_invocations"] = "7"
        summary = analyzer.analyze(data)
        row = summary["counters"][0]
        self.assertEqual(row["distribution"], dict(count=3, min=3, mean=5, p50=5, p95=6.8, p99=6.96, max=7))
        self.assertEqual(row["first"]["value"], 3)
        self.assertEqual(row["last"]["timestamp_ns"], 30)
        self.assertNotIn("sum", row)
        self.assertEqual(len(summary["counters"]), 2)  # same name on different threads stays separate
        gpu = summary["gpu_workloads"][0]
        self.assertEqual(gpu["count"], 2)
        self.assertEqual(gpu["command_count"]["max"], 10)
        self.assertEqual(gpu["draw_instance_invocations"]["max"], 7)
        self.assertEqual(gpu["draw_instance_invocations"]["count"], 1)  # absent legacy field is not a measured zero
        self.assertEqual(gpu["statuses"], dict(Available=1, Disjoint=1))
        other = copy.deepcopy(data)
        added = copy.deepcopy(other["spans"][0])
        added.update(id="7", name="1")
        other["spans"].append(added)
        comparison = analyzer.compare(data, other)
        changed = next(row for row in comparison["cpu_mean_per_complete_scope"] if row["name"] == "1")
        self.assertEqual((changed["count_a"], changed["count_b"]), (1, 2))
        self.assertEqual(comparison["counters_a"], summary["counters"])
        self.assertEqual(comparison["gpu_workloads_a"], summary["gpu_workloads"])

    def test_stale_epochs_are_not_current_event_loss(self):
        data = fixture()
        data["stale_epoch_events"] = 71
        summary = analyzer.analyze(data)
        self.assertEqual(summary["stale_epoch_events"], 71)
        row = next(row for row in summary["cpu"] if row["name"] == "1")
        self.assertEqual(row["exclusive_ns"], 40)
        comparison = analyzer.compare(data, data)
        self.assertEqual(comparison["stale_epoch_events_a"], 71)
        self.assertTrue(any(r["name"] == "1" for r in comparison["cpu_mean_per_complete_scope"]))
        data["stale_epoch_events"] = -1
        with self.assertRaises(ValueError):
            analyzer.analyze(data)

    def test_corrupt_and_schema_refused(self):
        for edit in (lambda d: d.update(schema=2), lambda d: d["spans"][0].update(end_ns=-1),
                     lambda d: d["counters"].append(dict(thread_id="1", timestamp_ns=0, name="bad", category="Counter",
                         context=d["spans"][0]["context"], value=float("nan"))),
                     lambda d: d["spans"].append(copy.deepcopy(d["spans"][0]))):
            data = fixture()
            edit(data)
            with self.assertRaises(ValueError):
                analyzer.analyze(data)

    def test_optional_string_table_references(self):
        data = fixture()
        data["strings"] = ["1", "CPU"]
        data["spans"][0].update(name_id=0, category_id=1)
        analyzer.analyze(data)
        for edit in (lambda d: d["spans"][0].update(name_id=2),
                     lambda d: d["spans"][0].update(name_id=-1),
                     lambda d: d["strings"].__setitem__(0, "wrong"),
                     lambda d: d.update(strings=None),
                     lambda d: d.pop("strings")):
            broken = copy.deepcopy(data)
            edit(broken)
            with self.assertRaises(ValueError):
                analyzer.analyze(broken)


if __name__ == "__main__":
    unittest.main()
