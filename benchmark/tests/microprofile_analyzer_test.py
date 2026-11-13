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
    def test_separate_catalog_loss_keeps_legacy_availability_explicit(self):
        data = fixture()
        summary = analyzer.analyze(data)
        self.assertIsNone(summary["pipeline_loss"]["dropped_pipeline_headers"])
        self.assertIsNone(summary["pipeline_loss"]["overwritten_pipeline_artifacts"])
        data["pipeline_loss_breakdown_available"] = True
        for field in analyzer.PIPELINE_LOSS_FIELDS[3:]:
            data[field] = 0
        data["overwritten_pipeline_artifacts"] = 7
        summary = analyzer.analyze(data)
        self.assertEqual(summary["pipeline_loss"]["dropped_pipeline_headers"], 0)
        self.assertEqual(summary["pipeline_loss"]["overwritten_pipeline_artifacts"], 7)
        for mutate in (lambda d: d.pop("dropped_pipeline_headers"),
                       lambda d: d.update(dropped_pipeline_headers=-1),
                       lambda d: d.update(pipeline_loss_breakdown_available="true")):
            broken = copy.deepcopy(data)
            mutate(broken)
            with self.assertRaises((ValueError, KeyError)):
                analyzer.validate(broken)

    def test_same_pass_name_keeps_renderer_and_view_identity(self):
        data = fixture()
        template = data["gpu_samples"][0]
        samples = []
        for renderer, view, duration, workload, fragments in (("7", "11", 900, 10, 100),
                                                              ("7", "12", 90, 20, 200),
                                                              ("8", "11", 30, 30, 300)):
            sample = copy.deepcopy(template)
            sample.update(name="Color Pass", end_ns=duration, command_count=str(workload))
            sample["context"] = {**sample["context"], "renderer_id": renderer, "view_id": view}
            sample["pipeline_statistics"] = {"status": "Available", "reason": "Native query",
                **{field: str(fragments) for field in analyzer.PIPELINE_COUNTER_FIELDS}}
            samples.append(sample)
        data["gpu_samples"] = samples
        result = analyzer.analyze(data)
        counters = {(row["renderer_id"], row["view_id"], row["name"]): row for row in result["gpu_pipeline_statistics"]}
        self.assertEqual(len(counters), 3)
        self.assertEqual([counters[(r, v, "Color Pass")]["fragment_shader_invocations"]["max"]
                          for r, v in (("7", "11"), ("7", "12"), ("8", "11"))], [100, 200, 300])
        self.assertTrue(all(row["query_kind"] == "NativePipelineStatistics" and row["timing_source_independent"]
                            for row in counters.values()))
        timings = {(row["renderer_id"], row["view_id"]): row for row in result["gpu_by_view"]}
        self.assertEqual([timings[(r, v)]["inclusive_distribution_ns"]["p50"]
                          for r, v in (("7", "11"), ("7", "12"), ("8", "11"))], [900, 90, 30])
        workloads = {(row["renderer_id"], row["view_id"]): row for row in result["workload_by_view"]}
        self.assertEqual([workloads[(r, v)]["command_count"]["p50"]
                          for r, v in (("7", "11"), ("7", "12"), ("8", "11"))], [10, 20, 30])
        self.assertEqual(len(result["gpu"]), 1)
        self.assertEqual(result["gpu"][0]["count"], 3)
        self.assertEqual(result["gpu"][0]["aggregation"], "AcrossViews")
        self.assertEqual(result["gpu_workloads"][0]["aggregation"], "AcrossViews")
        comparison = analyzer.compare(data, data)
        self.assertEqual(comparison["gpu_by_view_a"], result["gpu_by_view"])
        self.assertEqual(comparison["workload_by_view_a"], result["workload_by_view"])
        self.assertEqual(comparison["gpu_pipeline_statistics_a"], result["gpu_pipeline_statistics"])

    def test_pipeline_queries_are_independent_of_timestamp_results(self):
        data = fixture()
        values = {field: str(index) for index, field in enumerate(analyzer.PIPELINE_COUNTER_FIELDS)}
        values["fragment_shader_invocations"] = "18446744073709551615"
        data["gpu_samples"][0].update(status="Disjoint", pipeline_statistics={
            "status": "Available", "reason": "Hardware query", **values})
        data["gpu_samples"][1]["pipeline_statistics"] = {"status": "Pending", "reason": "Waiting without blocking"}
        summary = analyzer.analyze(data)
        row = summary["gpu_pipeline_statistics"][0]
        self.assertEqual(summary["gpu"], [])
        self.assertEqual(row["statuses"], {"Available": 1, "Pending": 1})
        self.assertEqual(row["input_assembly_vertices"]["min"], 0)
        self.assertEqual(row["fragment_shader_invocations"]["max"], (1 << 64) - 1)
        self.assertEqual(row["fragment_shader_invocations"]["p50"], (1 << 64) - 1)
        self.assertEqual(row["fragment_shader_invocations"]["count"], 1)
        self.assertEqual(row["unavailable_reasons"], {"Waiting without blocking": 1})
        for status in analyzer.SAMPLE_STATUSES[1:]:
            data["gpu_samples"][1]["pipeline_statistics"]["status"] = status
            self.assertEqual(analyzer.analyze(data)["gpu_pipeline_statistics"][0]["fragment_shader_invocations"]["count"], 1)
        data["gpu_samples"][0].pop("pipeline_statistics")
        row = analyzer.analyze(data)["gpu_pipeline_statistics"][0]
        self.assertIsNone(row["fragment_shader_invocations"])
        self.assertIn("Not recorded by this trace version", row["unavailable_reasons"])
        data["gpu_samples"][0]["pipeline_statistics"] = {"status": "Available", "reason": "query", **values}
        for fault in (lambda d: d["gpu_samples"][0]["pipeline_statistics"].pop("clipping_primitives"),
                      lambda d: d["gpu_samples"][0]["pipeline_statistics"].update(status="Unknown"),
                      lambda d: d["gpu_samples"][0]["pipeline_statistics"].update(fragment_shader_invocations="18446744073709551616"),
                      lambda d: d["gpu_samples"][0]["pipeline_statistics"].update(input_assembly_vertices=0)):
            broken = copy.deepcopy(data)
            fault(broken)
            with self.assertRaises((ValueError, KeyError)):
                analyzer.validate(broken)

    def test_compiled_pipeline_properties_are_typed_and_separate_from_timings(self):
        data = fixture()
        base = dict(context=data["spans"][0]["context"], kind="Statistic", pipeline_id="77", program_id="88", pass_id="1",
                    executable_index=0, stages=16, subgroup_size=16, program_name="heaven_lit [variant 23]",
                    name="Driver arbitrary statistic", description="Driver-provided property", status="Available",
                    reason="Driver extension", format="Uint64", original_size="0", truncated=False, text="")
        data["pipeline_events"] = [{**base, "format": fmt, "value": value} for fmt, value in (
            ("Uint64", "18446744073709551615"), ("Int64", "-9223372036854775808"), ("Bool32", True), ("Float64", 2.5))]
        data["pipeline_events"] += [{**base, "kind": "Use"}, {**base, "kind": "InternalRepresentation",
            "text": "driver IR", "original_size": "9000", "truncated": True}]
        data["pipeline_events"] += [{**base, "kind": "Loss", "status": "Dropped", "loss_count": str(count)} for count in (3, 4)]
        data.update(dropped_pipeline_events=3, overwritten_pipeline_events=5, truncated_pipeline_artifacts=1)
        summary = analyzer.analyze(data)
        self.assertEqual(len(summary["pipeline_executables"]), 7)
        self.assertEqual(summary["driver_pipeline_loss_observed"], 7)
        self.assertTrue(summary["driver_pipeline_loss_count_available"])
        self.assertEqual(len(summary["pipeline_uses"]), 1)
        self.assertEqual(summary["pipeline_executables"][0]["value"], "18446744073709551615")
        self.assertEqual(summary["pipeline_loss"]["overwritten_pipeline_events"], 5)
        self.assertEqual(summary["gpu"][0]["inclusive_ns"], 900)
        self.assertEqual(analyzer.compare(data, data)["pipeline_executables_a"], summary["pipeline_executables"])
        for index, value in ((0, "18446744073709551616"), (1, "-9223372036854775809"), (2, 1), (3, float("inf"))):
            broken = copy.deepcopy(data)
            broken["pipeline_events"][index]["value"] = value
            with self.assertRaises(ValueError):
                analyzer.validate(broken)
        broken = copy.deepcopy(data)
        broken["pipeline_events"][5]["text"] = "x" * 4097
        with self.assertRaisesRegex(ValueError, "overlong pipeline"):
            analyzer.validate(broken)
        broken = copy.deepcopy(data)
        broken["pipeline_events"][-1]["loss_count"] = "0"
        with self.assertRaisesRegex(ValueError, "loss delta"):
            analyzer.validate(broken)

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

    def test_gpu_diagnostic_mode_and_logical_enablement_must_match_for_comparison(self):
        ordinary = fixture()
        for metadata in ({"gpu_diagnostics_requested": "true"}, {"gpu_shader_ir_requested": "true"},
                         {"gpu_diagnostics": '{"pipeline_statistics_enabled":true,"executable_info_enabled":true}'}):
            diagnostic = copy.deepcopy(ordinary)
            diagnostic["metadata"].update(metadata)
            with self.assertRaisesRegex(ValueError, "identical nonempty recorded metadata"):
                analyzer.compare(ordinary, diagnostic)
            other = copy.deepcopy(diagnostic)
            key = next(iter(metadata))
            other["metadata"][key] = "false" if key.endswith("requested") else '{"pipeline_statistics_enabled":false,"executable_info_enabled":false}'
            with self.assertRaisesRegex(ValueError, "identical nonempty recorded metadata"):
                analyzer.compare(diagnostic, other)
            self.assertTrue(analyzer.compare(diagnostic, diagnostic)["cpu_mean_per_complete_scope"])

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

    def test_native_draw_availability_is_independent_of_timestamps(self):
        data = fixture()
        measured = data["gpu_samples"][0]
        measured.update(native_draw_commands_available=True, native_draw_commands_full="18446744073709551615",
                        native_draw_commands_2x1="7", native_draw_commands_2x2="11")
        row = analyzer.analyze(data)["gpu_workloads"][0]
        self.assertEqual(row["native_draw_commands_available_count"], 1)
        self.assertEqual(row["native_draw_commands_unavailable_count"], 1)
        self.assertEqual(row["native_draw_commands_full"]["max"], (1 << 64) - 1)
        self.assertEqual(row["native_draw_commands_2x1"]["count"], 1)
        self.assertEqual(row["native_draw_commands_2x2"]["mean"], 11)
        # A disjoint timer still carries observed native draw commands, including
        # real zeros; an older report's missing fields are not zero observations.
        measured.pop("native_draw_commands_available")
        unavailable_timer = data["gpu_samples"][1]
        unavailable_timer.update(native_draw_commands_available=True, native_draw_commands_full="0",
                                 native_draw_commands_2x1="0", native_draw_commands_2x2="0")
        row = analyzer.analyze(data)["gpu_workloads"][0]
        self.assertEqual(row["native_draw_commands_available_count"], 1)
        self.assertEqual(row["native_draw_commands_full"]["count"], 1)
        self.assertEqual(row["native_draw_commands_full"]["max"], 0)
        unavailable_timer["native_draw_commands_available"] = False
        row = analyzer.analyze(data)["gpu_workloads"][0]
        self.assertEqual(row["native_draw_commands_available_count"], 0)
        self.assertEqual(row["native_draw_commands_unavailable_count"], 2)
        self.assertIsNone(row["native_draw_commands_full"])

    def test_native_draws_remain_owned_by_each_pass(self):
        data = fixture()
        parent, child = data["gpu_samples"]
        parent.update(name="Parent", native_draw_commands_available=True, native_draw_commands_full="3",
                      native_draw_commands_2x1="0", native_draw_commands_2x2="0")
        child.update(name="Child", parent_pass_id=parent["pass_id"], native_draw_commands_available=True,
                     native_draw_commands_full="0", native_draw_commands_2x1="5", native_draw_commands_2x2="7")
        result = analyzer.analyze(data)
        rows = {row["name"]: row for row in result["gpu_workloads"]}
        self.assertEqual(rows["Parent"]["native_draw_commands_full"]["max"], 3)
        self.assertEqual(rows["Parent"]["native_draw_commands_2x2"]["max"], 0)
        self.assertEqual(rows["Child"]["native_draw_commands_2x2"]["max"], 7)
        self.assertNotIn("native_draw_commands_total", result)
        self.assertNotIn("sum", rows["Parent"]["native_draw_commands_full"])

    def test_invalid_native_draw_observations_are_rejected(self):
        for fault in ("missing", "flag", "numeric", "negative", "overflow"):
            with self.subTest(fault=fault):
                data = fixture()
                sample = data["gpu_samples"][0]
                sample.update(native_draw_commands_available=True, native_draw_commands_full="3",
                              native_draw_commands_2x1="5", native_draw_commands_2x2="7")
                if fault == "missing":
                    sample.pop("native_draw_commands_2x1")
                elif fault == "flag":
                    sample["native_draw_commands_available"] = "true"
                elif fault == "numeric":
                    sample["native_draw_commands_full"] = 3
                elif fault == "negative":
                    sample["native_draw_commands_full"] = "-1"
                else:
                    sample["native_draw_commands_full"] = str(1 << 64)
                with self.assertRaises((KeyError, ValueError)):
                    analyzer.analyze(data)

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
