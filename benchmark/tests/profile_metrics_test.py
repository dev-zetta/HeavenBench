#!/usr/bin/env python3
"""CPU-only semantic oracles for native profile selection and ABBA acceptance."""
import copy
from fractions import Fraction
import json
import math
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from profile_harness import metrics


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def profile(intervals=None):
    """Emit the float32 statistics written by the native two-second contract."""
    intervals = [1.0] * 182 if intervals is None else intervals
    rounded = [f32(value) for value in intervals]
    count, excluded = 0, 0.0
    for value in rounded:
        if excluded >= 2:
            break
        excluded += value
        count += 1
    retained = rounded[count:]
    seconds = math.fsum(retained)
    return {
        "schema": 1, "benchmark_score": False, "screenshot_readback": False,
        "overlay_visible": False, "floating_islands_frozen": False,
        "columns": list(metrics.COLUMNS),
        "measured_frames": len(retained), "measured_seconds": f32(seconds),
        "warmup_seconds": f32(excluded),
        "average_fps": f32(len(retained) / seconds) if seconds else 0,
        "samples_including_warmup": [[value, 0.01, 0.04, 0.06, 2.5, -1.0]
                                     for value in intervals],
    }


def cohort(a=(1000, 1004, 1003, 1002), b=(970, 971, 970, 972),
           frames=(400, 800, 200, 600, 500, 250, 1000, 700)):
    result = []
    positions = {"A": 0, "B": 0}
    for index, (variant, count) in enumerate(zip("ABBAABBA", frames)):
        means = a if variant == "A" else b
        mean = means[positions[variant]]
        positions[variant] += 1
        result.append({
            "id": f"cohort-{index // 4 + 1}/window-{index % 4 + 1}",
            "variant": variant,
            "signature": {"gpu": "fixture GPU", "driver": "fixture driver",
                          "api": "Vulkan", "resolution": [1920, 1080],
                          "settings": {"contact_shadows": "High", "sun_rays": 16,
                                       "depth_steps": 32, "ordinary_animation": True}},
            "statistics": {"measurement": {"frames": count,
                "seconds": count * mean / 1000, "mean_ms": mean, "fps": 1000 / mean}},
        })
    return result


class ProfileMetricsTest(unittest.TestCase):
    def test_exact_native_and_final_whole_interval_boundaries(self):
        data = profile()
        result = metrics.analyze_profile(data)
        self.assertEqual(result["native"]["frames"], 180)
        self.assertEqual(result["native"]["seconds"], 180)
        self.assertEqual(result["native"]["mean_ms"], 1000)
        self.assertEqual(result["measurement"]["frames"], 120)
        self.assertEqual(result["measurement"]["seconds"], 120)
        self.assertEqual(result["selection"]["start_index"], 62)
        self.assertEqual(result["selection"]["end_index"], 182)
        self.assertEqual(result["selection"]["excluded_prefix_seconds"], 62)

    def test_native_crossing_interval_is_excluded_whole(self):
        result = metrics.analyze_profile(profile([0.75, 1.5] + [1.0] * 180))
        self.assertEqual(result["native"]["frames"], 180)
        self.assertEqual(result["native"]["seconds"], 180)
        self.assertEqual(result["selection"]["excluded_prefix_seconds"], 62.25)

    def test_float32_rounding_controls_native_boundary(self):
        # 200 native float32 0.01s intervals sum to <2s, so the next 1s
        # interval is excluded too. Decimal/double input arithmetic differs.
        data = profile([0.01] * 200 + [1.0] * 180)
        self.assertLess(math.fsum([f32(0.01)] * 200), 2)
        result = metrics.analyze_profile(data)
        self.assertEqual(result["native"]["frames"], 179)
        self.assertEqual(result["native"]["seconds"], 179)
        self.assertEqual(result["measurement"]["frames"], 120)
        self.assertEqual(result["selection"]["start_index"], 260)

    def test_exact_sixty_second_prefix_qualifies(self):
        result = metrics.analyze_profile(profile([60.0] + [1.0] * 120))
        self.assertEqual(result["selection"]["excluded_prefix_seconds"], 60)
        self.assertEqual(result["selection"]["start_index"], 1)
        self.assertEqual(result["measurement"]["seconds"], 120)
        with self.assertRaises(ValueError):
            metrics.analyze_profile(profile([59.999] + [1.0] * 120))

    def test_suffix_crossing_and_very_slow_frame_are_never_trimmed(self):
        result = metrics.analyze_profile(profile([60.0] + [11.0] * 11))
        self.assertEqual(result["measurement"]["seconds"], 121)
        self.assertEqual(result["measurement"]["frames"], 11)
        self.assertEqual(result["measurement"]["p99_ms"], 11000)
        result = metrics.analyze_profile(profile([60.0, 1.0, 2.0, 200.0]))
        self.assertEqual(result["selection"]["start_index"], 3)
        self.assertEqual(result["selection"]["excluded_prefix_seconds"], 63)
        self.assertEqual(result["measurement"]["seconds"], 200)
        self.assertEqual(result["measurement"]["frames"], 1)

    def test_insufficient_window_and_native_samples_are_rejected(self):
        for data in [profile([1.0] * 179), profile([180.0]), profile([0.5, 0.5])]:
            with self.subTest(samples=len(data["samples_including_warmup"])):
                with self.assertRaises(ValueError):
                    metrics.analyze_profile(data)
        data = profile()
        for rows in [None, [], "samples", {}, [1.0], [[1.0] * 5], [[1.0] * 7]]:
            with self.subTest(rows=rows):
                data["samples_including_warmup"] = rows
                with self.assertRaises(ValueError):
                    metrics.analyze_profile(data)

    def test_invalid_sample_numbers_and_booleans(self):
        for column in range(6):
            for value in [True, False, None, "1", float("nan"), float("inf"), -float("inf")]:
                with self.subTest(column=column, value=value):
                    data = profile()
                    data["samples_including_warmup"][80][column] = value
                    with self.assertRaises(ValueError):
                        metrics.analyze_profile(data)
        for value in [0.0, -0.1, 1e-50, 1e50]:
            with self.subTest(interval=value):
                data = profile()
                data["samples_including_warmup"][80][0] = value
                with self.assertRaises(ValueError):
                    metrics.analyze_profile(data)

    def test_schema_columns_and_capture_metadata_are_strict(self):
        for value in [None, True, False, 1.0, "1", 0, 2]:
            data = profile()
            data["schema"] = value
            with self.subTest(schema=value):
                with self.assertRaises(ValueError):
                    metrics.analyze_profile(data)
        data = profile()
        data["columns"] = data["columns"][::-1]
        with self.assertRaises(ValueError):
            metrics.analyze_profile(data)
        for key in ["benchmark_score", "screenshot_readback", "overlay_visible", "floating_islands_frozen"]:
            for value in [None, True, 0, "false"]:
                with self.subTest(key=key, value=value):
                    data = profile()
                    data[key] = value
                    with self.assertRaises(ValueError):
                        metrics.analyze_profile(data)
            data = profile()
            del data[key]
            with self.assertRaises(ValueError):
                metrics.analyze_profile(data)

    def test_native_statistics_must_match_actual_samples(self):
        for key, value in [("measured_frames", 179), ("measured_seconds", 179),
                           ("average_fps", 2), ("warmup_seconds", 1)]:
            data = profile()
            data[key] = value
            with self.subTest(key=key):
                with self.assertRaises(ValueError):
                    metrics.analyze_profile(data)
        for key in ["measured_seconds", "average_fps", "warmup_seconds"]:
            for value in [True, None, "180", float("nan"), float("inf")]:
                with self.subTest(key=key, value=value):
                    data = profile()
                    data[key] = value
                    with self.assertRaises(ValueError):
                        metrics.analyze_profile(data)
        for data, bad_count in [(profile(), 180.0), (profile([60.0, 120.0]), True)]:
            data["measured_frames"] = bad_count
            with self.subTest(frame_count=bad_count):
                with self.assertRaises(ValueError):
                    metrics.analyze_profile(data)

    def test_invalid_declared_windows(self):
        for key, values in [("warmup_seconds", [True, 1.99, -1, None, float("nan"), float("inf")]),
                            ("window_seconds", [True, 59.99, -1, None, float("nan"), float("inf")])]:
            for value in values:
                with self.subTest(key=key, value=value):
                    with self.assertRaises(ValueError):
                        metrics.analyze_profile(profile(), **{key: value})

    def test_nearest_rank_percentiles_from_known_distribution(self):
        result = metrics.summary([0.001 * v for v in range(1, 101)])
        self.assertAlmostEqual(result["mean_ms"], 50.5)
        self.assertAlmostEqual(result["p50_ms"], 50)
        self.assertAlmostEqual(result["p95_ms"], 95)
        self.assertAlmostEqual(result["p99_ms"], 99)

    def test_gpu_delay_and_cpu_stages_do_not_infer_aligned_or_summed_cost(self):
        data = profile()
        expected = metrics.analyze_profile(data)
        for index, row in enumerate(data["samples_including_warmup"]):
            row[1:5] = [1e100, 2e100, 3e100, 4e100]
            row[5] = [-1, 30000, 30000, 0][index % 4]
        before = copy.deepcopy(data)
        result = metrics.analyze_profile(data)
        self.assertEqual(data, before, "analysis must not mutate raw samples")
        self.assertEqual(result, expected, "asynchronous samples cannot affect interval statistics")
        self.assertIn("asynchronous", result["gpu_note"])
        self.assertIn("no CPU/GPU sum or alignment", result["gpu_note"])
        self.assertNotIn("30000", json.dumps(result))
        self.assertNotIn("1e+100", json.dumps(result))


class AbbaMetricsTest(unittest.TestCase):
    def test_two_quartets_use_pooled_seconds_and_frame_counts(self):
        data = cohort()
        before = copy.deepcopy(data)
        result = metrics.compare_abba(data)
        self.assertTrue(result["passed"], result)
        expected = {}
        for variant in ["A", "B"]:
            matching = [x for x in data if x["variant"] == variant]
            count = sum(x["statistics"]["measurement"]["frames"] for x in matching)
            expected[variant] = sum(Fraction(x["statistics"]["measurement"]["mean_ms"])
                                    * x["statistics"]["measurement"]["frames"] for x in matching) / count
            self.assertAlmostEqual(result["variants"][variant]["mean_ms"], float(expected[variant]))
            self.assertAlmostEqual(result["variants"][variant]["fps"], 1000 / float(expected[variant]))
        self.assertAlmostEqual(result["improvement_percent"], float((1 - expected["B"] / expected["A"]) * 100))
        self.assertNotAlmostEqual(result["variants"]["A"]["mean_ms"], sum([1000, 1004, 1003, 1002]) / 4)
        self.assertEqual(data, before)
        self.assertEqual(len(result["quartet_improvements_percent"]), 2)
        self.assertTrue(all(v > 0 for v in result["quartet_improvements_percent"]))

    def test_baseline_repeatability_cap_is_independent_of_large_gain(self):
        result = metrics.compare_abba(cohort(a=[1000, 1020, 1000, 1000], b=[900] * 4))
        self.assertGreater(result["improvement_percent"], result["noise_gate_percent"])
        self.assertGreater(result["variants"]["A"]["range_percent"], 1)
        self.assertFalse(result["passed"])

    def test_candidate_repeat_range_blocks_small_pooled_gain(self):
        result = metrics.compare_abba(cohort(a=[1000] * 4, b=[900, 1030, 900, 1030], frames=[400] * 8))
        self.assertGreater(result["improvement_percent"], 0)
        self.assertGreater(result["variants"]["B"]["range_percent"], result["improvement_percent"])
        self.assertFalse(result["passed"])

    def test_first_last_drift_is_reported_and_gates_small_gain(self):
        result = metrics.compare_abba(cohort(a=[1000, 1000, 1000, 1008], b=[999] * 4, frames=[400] * 8))
        self.assertAlmostEqual(result["baseline_first_last_drift_percent"], 8 / 1002 * 100)
        self.assertGreater(result["noise_gate_percent"], result["improvement_percent"])
        self.assertFalse(result["passed"])

    def test_no_gain_is_not_accepted_even_with_zero_noise(self):
        result = metrics.compare_abba(cohort(a=[1000] * 4, b=[1000] * 4))
        self.assertEqual(result["improvement_percent"], 0)
        self.assertEqual(result["noise_gate_percent"], 0)
        self.assertFalse(result["passed"])
        result = metrics.compare_abba(cohort(a=[1000] * 4, b=[1010] * 4))
        self.assertLess(result["improvement_percent"], 0)
        self.assertFalse(result["passed"])

    def test_both_quartet_signs_are_retained(self):
        result = metrics.compare_abba(cohort(a=[1000] * 4, b=[1001, 1001, 990, 990], frames=[400] * 8))
        self.assertLess(result["quartet_improvements_percent"][0], 0)
        self.assertGreater(result["quartet_improvements_percent"][1], 0)
        self.assertFalse(result["passed"])

    def test_duplicate_missing_extra_and_reordered_windows_are_rejected(self):
        variants = []
        data = cohort(); data[1]["id"] = data[0]["id"]; variants.append(data)
        variants.extend([cohort()[:4], cohort()[:7], cohort() + [copy.deepcopy(cohort()[0])]])
        data = cohort(); data[0], data[1] = data[1], data[0]; variants.append(data)
        for data in variants:
            with self.subTest(order=[x["variant"] for x in data]):
                with self.assertRaises(ValueError):
                    metrics.compare_abba(data)

    def test_workload_hardware_driver_and_settings_mismatches_are_rejected(self):
        for key, value in [("gpu", "other GPU"), ("driver", "other driver"),
                           ("api", "OpenGL"), ("resolution", [1280, 720]),
                           ("settings", {"contact_shadows": "Low"})]:
            data = cohort(); data[5]["signature"][key] = value
            with self.subTest(field=key):
                with self.assertRaises(ValueError):
                    metrics.compare_abba(data)

    def test_invalid_gate_values_are_rejected(self):
        for value in [True, False, None, "1", -1, float("nan"), float("inf")]:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    metrics.compare_abba(cohort(), max_baseline_range_percent=value)

    def test_invalid_or_inconsistent_measurement_statistics_are_rejected(self):
        for key, values in [("frames", [True, 0, -1, 400.0, float("nan")]),
                            ("seconds", [True, 0, -1, float("nan"), float("inf")]),
                            ("mean_ms", [True, 0, -1, float("nan"), float("inf")]),
                            ("fps", [True, 0, -1, float("nan"), float("inf")])]:
            for value in values:
                with self.subTest(field=key, value=value):
                    data = cohort(); data[0]["statistics"]["measurement"][key] = value
                    with self.assertRaises(ValueError):
                        metrics.compare_abba(data)
        data = cohort(); data[0]["statistics"]["measurement"]["seconds"] += 100
        with self.assertRaises(ValueError):
            metrics.compare_abba(data)


if __name__ == "__main__":
    unittest.main()
