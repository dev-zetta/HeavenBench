#!/usr/bin/env python3
"""CPU-only controller integration: recursive evidence and no-retry boundaries."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import shutil
import subprocess
import time
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import profile_compare as harness
from profile_harness.monitor import file_identity, mapped_file_identity

ORDER = ["prime-A", "prime-B", "pilot-1", "pilot-2", "A1", "B1", "B2", "A2", "A3", "B3", "B4", "A4"]


def monitoring_contract():
    return {"mode": "read-only sysfs worker, up to 1 Hz; observation can affect performance",
            "clock": "host monotonic observations, not GPU clock calibration", "power_limit_w": 275.0,
            "device_vram_total_mb": 16384.0, "gpu_high_c": 90.0, "gpu_critical_c": 100.0,
            "vram_high_c": None, "vram_critical_c": None}


def sensor_observations():
    return {"sample_monotonic_ns": 123456, "available": True, "reason": "Available",
            "gpu_temperature_c": 60.0, "vram_temperature_c": 65.0, "power_w": 140.0,
            "graphics_clock_mhz": 2800.0, "memory_clock_mhz": None, "fan_rpm": 0.0,
            "utilization_percent": None, "memory_utilization_percent": None,
            "device_vram_used_mb": 4096.0, "throttle_reason": None}


class HardwareMonitoringTest(unittest.TestCase):
    def setUp(self):
        self.contract = monitoring_contract()
        self.spec = {"hardware_monitoring": self.contract, "expected_profile": {},
                     "gpu_id": "8086:e223", "pci": "0000:03:00.0", "expected_geometry": {"triangles": 10}}
        self.profile = {"api": "Vulkan", "gpu": "Intel", "driver": "Mesa", "driver_version": "26",
                        "width": 1920, "height": 1080, "camera_position": [1, 2, 3], "camera_yaw": 0,
                        "camera_pitch": 0, "camera_fov": 60, "render_resolution_scale": [1, 1],
                        "renderer_features": {}, "runtime_settings": {},
                        "hardware_telemetry": {**self.contract, **sensor_observations()}}
        self.text = "\n".join(["GPU selection verified: 8086:e223 at 0000:03:00.0",
                               "Window: SDL video driver = x11", "FilamentBackend: native window = X11",
                               "Window: drawable size = 1920x1080", "Prepared displacement cache: hit"])

    def signature(self, profile=None, spec=None):
        with patch.object(harness, "geometry", return_value={"triangles": 10}):
            return harness.signature(profile or self.profile, {"width": 1920, "height": 1080},
                                     spec or self.spec, self.text)

    def test_readings_vary_but_stable_contract_is_identity_and_observations_are_disclosed(self):
        other = deepcopy(self.profile)
        other["hardware_telemetry"].update({"sample_monotonic_ns": 987654, "gpu_temperature_c": 80.0,
                                            "power_w": None, "fan_rpm": 1234, "available": False,
                                            "reason": "Temporarily unavailable", "throttle_reason": "driver report"})
        self.assertEqual(self.signature(), self.signature(other))
        self.assertEqual(self.signature()["hardware_monitoring"], self.contract)
        left_contract, left = harness.hardware_monitoring(self.profile, self.spec)
        right_contract, right = harness.hardware_monitoring(other, self.spec)
        self.assertEqual(left_contract, right_contract)
        self.assertNotEqual(left, right)
        self.assertEqual(left["fan_rpm"], 0)
        self.assertIsNone(right["power_w"])
        self.assertEqual(right["gpu_temperature_c"], 80)

    def test_missing_metadata_and_stable_mode_cap_capacity_or_limits_mismatch_reject(self):
        for field, value in (("mode", "disabled"), ("clock", "GPU-aligned"), ("power_limit_w", 200),
                             ("device_vram_total_mb", 8192), ("gpu_high_c", 85), ("gpu_critical_c", 99),
                             ("vram_high_c", 90), ("vram_critical_c", 100)):
            other = deepcopy(self.profile)
            other["hardware_telemetry"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "stable contract differs"):
                self.signature(other)
        for metadata in (None, {}, {**self.profile["hardware_telemetry"], "unknown_monitor_policy": "new"}):
            other = {**self.profile, "hardware_telemetry": metadata}
            with self.subTest(metadata=metadata), self.assertRaisesRegex(ValueError, "hardware monitoring metadata"):
                self.signature(other)

    def test_invalid_contract_rejects_before_cohort_mutation(self):
        invalid = [None, {}, {**self.contract, "unknown": True}, {**self.contract, "mode": ""},
                   {**self.contract, "clock": False}, {**self.contract, "power_limit_w": True},
                   {**self.contract, "gpu_high_c": float("nan")}, {**self.contract, "gpu_critical_c": -1}]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for i, contract in enumerate(invalid):
                path = root / f"spec-{i}.json"
                path.write_text(json.dumps({"schema": 1, "hardware_monitoring": contract}))
                cohort = root / f"absent-{i}/cohort"
                with self.subTest(contract=contract), self.assertRaisesRegex(ValueError, "hardware_monitoring"):
                    harness.prepare(path, cohort)
                self.assertFalse(cohort.parent.exists())

    def test_invalid_observations_do_not_silently_become_ignored_data(self):
        for field, value in (("sample_monotonic_ns", True), ("sample_monotonic_ns", 1 << 64),
                             ("available", 1), ("reason", None), ("throttle_reason", 0),
                             ("gpu_temperature_c", float("inf")), ("fan_rpm", -1), ("power_w", True)):
            other = deepcopy(self.profile)
            other["hardware_telemetry"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.signature(other)

    def test_legacy_protocol_behavior_remains_unchanged(self):
        legacy = {k: v for k, v in self.spec.items() if k != "hardware_monitoring"}
        invalid = {**self.profile, "hardware_telemetry": {"legacy_observation": "not part of old signature"}}
        self.assertEqual(self.signature(spec=legacy), self.signature(invalid, legacy))
        self.assertNotIn("hardware_monitoring", self.signature(spec=legacy))


class CachedSeedOrderTest(unittest.TestCase):
    def test_imported_cache_requires_ten_new_windows_and_no_prime_override(self):
        spec = {"order": ORDER[2:], "shader_seed_directory": "/seed",
                "shader_seed_provenance": {}, "profile_seconds": 260}
        self.assertEqual(harness.prospective_order(spec), ORDER[2:])
        self.assertEqual(harness.requested_profile_seconds(spec, "pilot-1"), 260)
        for change in ({"order": ORDER}, {"prime_seconds": 260}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                harness.prospective_order({**spec, **change})

    def test_cache_metadata_cannot_be_silently_ignored_or_invent_priming(self):
        for spec in ({"order": ORDER, "shader_seed_provenance": {}},
                     {"order": ORDER[2:], "shader_seed_directory": "/seed"},
                     {"order": ORDER[2:]}):
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                harness.prospective_order(spec)


class ProfileDurationTest(unittest.TestCase):
    def test_default_priming_preserves_existing_durations(self):
        spec = {"profile_seconds": 185}
        for label in ORDER:
            with self.subTest(label=label):
                self.assertEqual(harness.requested_profile_seconds(spec, label),
                                 65 if label in ("prime-A", "prime-B") else 185)

    def test_longer_priming_override_leaves_pilot_and_comparison_durations(self):
        for prime_seconds in (65, 120, 185):
            for label in ORDER:
                with self.subTest(prime_seconds=prime_seconds, label=label):
                    spec = {"profile_seconds": 185, "prime_seconds": prime_seconds}
                    self.assertEqual(harness.requested_profile_seconds(spec, label),
                                     prime_seconds if label in ("prime-A", "prime-B") else 185)
        # A distinct profile duration catches accidentally applying the prime
        # override to pilot/comparison labels when both happen to be 185.
        spec = {"profile_seconds": 245, "prime_seconds": 185}
        for label in ORDER[2:]:
            self.assertEqual(harness.requested_profile_seconds(spec, label), 245)

    def test_prime_duration_requires_integer_and_declared_bounds(self):
        invalid = (None, True, False, "185", 185.0, float("nan"), float("inf"),
                   float("-inf"), [], {}, -1, 0, 64, 186)
        for value in invalid:
            with self.subTest(prime_seconds=repr(value)):
                with self.assertRaisesRegex(ValueError, "prime_seconds"):
                    harness.requested_profile_seconds({"profile_seconds": 185, "prime_seconds": value}, "prime-A")

    def test_invalid_prime_specs_fail_before_any_cohort_directory_mutation(self):
        invalid = (None, True, False, "185", 185.0, float("nan"), float("inf"),
                   float("-inf"), [], {}, -1, 0, 64, 186)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, value in enumerate(invalid):
                with self.subTest(prime_seconds=repr(value)):
                    spec = {"schema": 1, "order": ORDER, "profile_seconds": 185,
                            "prime_seconds": value, "warmup_seconds": 60, "window_seconds": 120,
                            "max_native_runs": len(ORDER), "max_wall_seconds": 3406}
                    path = root / f"spec-{index}.json"
                    path.write_text(json.dumps(spec))
                    cohort = root / f"new-parent-{index}" / "cohort"
                    with patch.object(harness, "store_open") as store, \
                            patch.object(harness, "host_identity") as host, \
                            patch.object(harness, "run_owned") as native:
                        with self.assertRaisesRegex(ValueError, "prime_seconds"):
                            harness.prepare(path, cohort)
                        store.assert_not_called()
                        host.assert_not_called()
                        native.assert_not_called()
                    self.assertFalse(cohort.parent.exists())
                    self.assertFalse(cohort.exists())

    def test_invalid_profile_upper_bound_is_rejected_without_directories(self):
        for index, duration in enumerate((None, True, False, "185", float("nan"), float("inf"), 64)):
            with tempfile.TemporaryDirectory() as temporary, self.subTest(profile_seconds=repr(duration)):
                root = Path(temporary)
                path = root / "spec.json"
                path.write_text(json.dumps({"schema": 1, "order": ORDER,
                                           "profile_seconds": duration, "prime_seconds": 65}))
                cohort = root / f"absent-{index}" / "cohort"
                with self.assertRaisesRegex(ValueError, "prime_seconds"):
                    harness.prepare(path, cohort)
                self.assertFalse(cohort.parent.exists())


class CachedSeedRecoveryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.previous = self.root / "previous"
        self.previous.mkdir()
        self.seed = self.root / "exported-seed"
        (self.seed / "mesa").mkdir(parents=True)
        (self.seed / "mesa/cache").write_bytes(b"qualified shader bytes")
        geometry = self.root / "geometry"
        geometry.mkdir()
        (geometry / "geometry.zip").write_bytes(b"fixed geometry")
        config = self.root / "config.json"
        config.write_text(json.dumps({"width": 1920, "height": 1080, "quality": "High"}))
        binaries = {}
        for variant in ("A", "B"):
            path = self.root / (variant + "-binary")
            path.write_bytes(variant.encode())
            binaries[variant] = file_identity(path)
        self.spec = {"schema": 1, "order": ORDER[2:], "profile_seconds": 260,
                     "warmup_seconds": 75, "window_seconds": 180, "timeout_seconds": 335,
                     "max_native_runs": 10, "max_wall_seconds": 6500,
                     "max_baseline_range_percent": 1.0, "binaries": binaries,
                     "assets": [], "config": str(config), "geometry_seed": str(geometry),
                     "provenance": [], "pci": "test", "driver_path": "test", "data_dir": str(self.root),
                     "neutral_cache_name": "geometry.zip", "expected_profile": {"quality": "High"},
                     "expected_geometry": {"triangles": 10}, "camera_pose": "fixed-camera",
                     "shader_seed_directory": str(self.seed)}
        with harness.store_open(self.previous, create=True) as store:
            blob_binaries = {name: store.put_file(item["path"]) for name, item in binaries.items()}
            tree = harness.snapshot_tree(store, self.seed)
            old_spec = {key: deepcopy(self.spec[key]) for key in ("expected_profile", "expected_geometry", "camera_pose")}
            self.old = {"cohort_id": "old-cohort", "binaries": blob_binaries, "assets": [], "host": {},
                        "expected_config": json.loads(config.read_text()), "spec": old_spec,
                        "geometry_seed": harness.snapshot_tree(store, geometry)}
            self.old_ref = harness.seal(store, "protocol", "experiment-protocol", self.old,
                                       list(blob_binaries.values()) + [self.old["geometry_seed"]])
            self.window = {"id": "prime-B", "variant": "B", "passed": True,
                           "process": {"passed": True, "drain": {"passed": True}}, "ownership": {},
                           "cohort_id": "old-cohort", "statistics": {"foreign-timing": "must not be copied"}}
            window_ref = store.put_record("native-window", self.window, [self.old_ref])
            seed_ref = harness.seal(store, "combined-shader-seed", "shader-cache-seed", {"producer": "prime-B"}, [tree, window_ref])
            inventory = [{"name": item["name"], "bytes": item["ref"]["bytes"], "sha256": item["ref"]["sha256"]}
                         for item in store.verify(tree)["payload"]["files"]]
        self.spec["shader_seed_provenance"] = {"cohort": str(self.previous), "entry": "combined-shader-seed",
                                              "ref": seed_ref, "inventory": inventory}
        self.cohort = self.root / "recovery"
        self.spec_file = self.root / "spec.json"

    def prepare(self):
        self.spec_file.write_text(json.dumps(self.spec))
        with patch.object(harness, "host_identity", return_value={}), \
                patch.object(harness, "harness_inputs", return_value=[]), \
                patch.object(harness, "run_owned") as native:
            result = harness.prepare(self.spec_file, self.cohort)
            native.assert_not_called()
        return result

    def test_prepare_snapshots_only_artifact_and_declares_no_new_priming(self):
        self.assertEqual(self.prepare()["native_runs"], 0)
        with harness.store_open(self.cohort) as store:
            _, protocol = harness.record(store, "protocol")
            seed_ref, provenance = harness.record(store, "imported-shader-seed")
            self.assertEqual(protocol["shader_cache"], {"mode": "imported_artifact", "native_priming_planned": False, "seed_ref": seed_ref})
            self.assertFalse(provenance["native_priming_performed"])
            self.assertEqual(len(store.verify(seed_ref)["refs"]), 1)
            self.assertFalse((store.root / "objects" / self.old_ref["sha256"]).exists())
            self.assertFalse((store.root / "entries/windows").exists())
            self.assertFalse(store.exists("combined-shader-seed"))
        self.assertEqual(sorted(path.name for path in (self.cohort / "work").iterdir()), sorted(ORDER[2:]))

    def test_first_pilot_gets_private_pinned_bytes_after_external_seed_changes(self):
        self.prepare()
        (self.seed / "mesa/cache").write_bytes(b"changed external cache")
        with patch.object(harness, "host_identity", return_value={}), \
                patch.object(harness, "harness_inputs", return_value=[]), \
                patch.object(harness, "processes", return_value={"blockers": []}), \
                patch.object(harness, "gpu_observation", return_value={}), \
                patch.object(harness, "run_owned", side_effect=RuntimeError("CPU boundary only")) as native:
            with self.assertRaisesRegex(RuntimeError, "CPU boundary only"):
                harness.run(self.cohort, "pilot-1")
            native.assert_called_once()
        self.assertEqual((self.cohort / "work/pilot-1/cache/xdg/mesa/cache").read_bytes(), b"qualified shader bytes")
        with harness.store_open(self.cohort) as store:
            _, attempt = harness.record(store, "attempts/pilot-1")
            self.assertFalse(attempt["shader_cache"]["new_native_priming_launch"])
            self.assertEqual(attempt["shader_cache"]["mode"], "imported_artifact")
            self.assertFalse(store.exists("windows/prime-A"))
            self.assertFalse(store.exists("windows/prime-B"))

    def test_supplied_seed_tamper_ref_and_inventory_mismatch_are_rejected(self):
        original = deepcopy(self.spec)
        for change in ("bytes", "ref", "inventory"):
            with self.subTest(change=change):
                self.spec = deepcopy(original)
                self.cohort = self.root / ("reject-" + change)
                (self.seed / "mesa/cache").write_bytes(b"qualified shader bytes")
                if change == "bytes":
                    (self.seed / "mesa/cache").write_bytes(b"tampered")
                elif change == "ref":
                    self.spec["shader_seed_provenance"]["ref"]["sha256"] = "0" * 64
                else:
                    self.spec["shader_seed_provenance"]["inventory"][0]["bytes"] += 1
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse((self.cohort / "store/entries/protocol").exists())

    def test_source_hardware_binary_workload_or_geometry_mismatch_is_rejected(self):
        original = deepcopy(self.spec)
        for change in ("binary", "settings", "geometry", "camera"):
            with self.subTest(change=change):
                self.spec = deepcopy(original)
                self.cohort = self.root / ("mismatch-" + change)
                if change == "binary":
                    self.spec["binaries"]["A"] = self.spec["binaries"]["B"]
                elif change == "settings":
                    self.spec["expected_profile"] = {"quality": "Low"}
                elif change == "geometry":
                    self.spec["expected_geometry"] = {"triangles": 9}
                else:
                    self.spec["camera_pose"] = "different-camera"
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse((self.cohort / "store/entries/protocol").exists())

    def test_imported_cache_never_substitutes_for_new_pilots_or_comparisons(self):
        self.prepare()
        with patch.object(harness, "harness_inputs", return_value=[]):
            with self.assertRaises(ValueError):
                harness.analyze(self.cohort)
        self.assertFalse((self.cohort / "report.json").exists())

    def test_monitored_seed_import_requires_source_monitoring_qualification(self):
        self.spec["hardware_monitoring"] = monitoring_contract()
        with self.assertRaisesRegex(ValueError, "shader seed monitoring condition"):
            self.prepare()
        self.assertFalse((self.cohort / "store/entries/protocol").exists())

    def test_hardware_and_native_producer_qualification_cannot_be_faked(self):
        original = deepcopy(self.spec)
        for change in ("hardware", "failed-producer", "undrained", "unowned", "foreign-producer"):
            with self.subTest(change=change):
                self.spec = deepcopy(original)
                source = self.root / ("source-" + change)
                source.mkdir()
                self.cohort = self.root / ("bad-source-" + change)
                old = deepcopy(self.old)
                producer = deepcopy(self.window)
                if change == "hardware":
                    old["host"] = {"driver": "different"}
                elif change == "failed-producer":
                    producer["passed"] = False
                elif change == "undrained":
                    producer["process"]["drain"]["passed"] = False
                elif change == "unowned":
                    producer["ownership"] = None
                else:
                    producer["cohort_id"] = "another-cohort"
                with harness.store_open(source, create=True) as store:
                    binaries = [store.put_file(item["path"]) for item in self.spec["binaries"].values()]
                    geometry = harness.snapshot_tree(store, Path(self.spec["geometry_seed"]))
                    protocol_ref = harness.seal(store, "protocol", "experiment-protocol", old, binaries + [geometry])
                    window_ref = store.put_record("native-window", producer, [protocol_ref])
                    tree = harness.snapshot_tree(store, self.seed)
                    ref = harness.seal(store, "combined-shader-seed", "shader-cache-seed", {"producer": "prime-B"}, [tree, window_ref])
                self.spec["shader_seed_provenance"]["cohort"] = str(source)
                self.spec["shader_seed_provenance"]["ref"] = ref
                with self.assertRaises(ValueError):
                    self.prepare()
                self.assertFalse((self.cohort / "store/entries/protocol").exists())

    def new_windows(self, pilot_means=(120, 120), foreign=False):
        with harness.store_open(self.cohort) as store:
            protocol_ref, protocol = harness.record(store, "protocol")
            for label in ORDER[2:]:
                variant = "B" if label.startswith("B") else "A"
                frames = 1200 if variant == "B" else 1000
                mean = pilot_means[int(label[-1]) - 1] if label.startswith("pilot-") else 120000 / frames
                payload = {"id": label, "variant": variant, "passed": True,
                           "cohort_id": "old-cohort" if foreign else protocol["cohort_id"],
                           "process": {"elapsed_seconds": 1},
                           "signature": {"hardware": "fixed", "quality": "High"},
                           "statistics": {"measurement": {"frames": frames, "seconds": mean * frames / 1000,
                                                          "mean_ms": mean, "fps": 1000 / mean}}}
                harness.seal(store, f"windows/{label}", "native-window", payload, [protocol_ref])

    def test_ten_new_windows_analyze_without_inventing_priming(self):
        self.prepare()
        self.new_windows()
        with patch.object(harness, "harness_inputs", return_value=[]):
            result = harness.analyze(self.cohort)
        self.assertTrue(result["passed"])
        self.assertEqual(result["shader_cache"]["mode"], "imported_artifact")
        self.assertFalse(result["shader_cache"]["native_priming_planned"])
        self.assertFalse((self.cohort / "store/entries/windows/prime-A").exists())

    def test_foreign_windows_cannot_supply_new_cohort_metrics(self):
        self.prepare()
        self.new_windows(foreign=True)
        with patch.object(harness, "harness_inputs", return_value=[]):
            with self.assertRaisesRegex(ValueError, "foreign window"):
                harness.analyze(self.cohort)
        self.assertFalse((self.cohort / "report.json").exists())

    def test_seeded_A1_pilot_gate_still_precedes_attempt_and_native_launch(self):
        self.prepare()
        # Only the two new pilots exist, so A1 is at index two rather than four.
        with harness.store_open(self.cohort) as store:
            protocol_ref, protocol = harness.record(store, "protocol")
            for label, mean in (("pilot-1", 100), ("pilot-2", 110)):
                harness.seal(store, f"windows/{label}", "native-window", {
                    "id": label, "cohort_id": protocol["cohort_id"], "passed": True,
                    "process": {"elapsed_seconds": 1}, "statistics": {"measurement": {"mean_ms": mean}}}, [protocol_ref])
        with patch.object(harness, "host_identity", return_value={}), \
                patch.object(harness, "harness_inputs", return_value=[]), \
                patch.object(harness, "run_owned") as native:
            with self.assertRaisesRegex(ValueError, "pilot variance"):
                harness.run(self.cohort, "A1")
            native.assert_not_called()
        self.assertFalse((self.cohort / "store/entries/attempts/A1").exists())
        self.assertFalse((self.cohort / "work/A1/cache/xdg").exists())

    def test_imported_closure_corruption_prevents_any_launch(self):
        self.prepare()
        with harness.store_open(self.cohort) as store:
            ref, _ = harness.record(store, "imported-shader-seed")
            tree = store.verify(store.verify(ref)["refs"][0])
            blob = store.root / "objects" / tree["payload"]["files"][0]["ref"]["sha256"]
            blob.chmod(0o600)
            blob.write_bytes(b"corrupted immutable cache")
        with patch.object(harness, "run_owned") as native:
            with self.assertRaises(ValueError):
                harness.run(self.cohort, "pilot-1")
            native.assert_not_called()


class ControllerTest(unittest.TestCase):
    def declare_power(self):
        self.store.close()
        self.cohort = self.root / 'power-cohort'
        self.cohort.mkdir()
        self.store = harness.store_open(self.cohort, create=True)
        self.addCleanup(self.store.close)
        self.protocol['spec']['power_observer'] = {'identity': {'sha256': 'fixed-tool'}}
        self.protocol_ref = harness.seal(self.store, 'protocol', 'experiment-protocol', self.protocol)

    @patch.object(harness, 'harness_inputs', return_value=[])
    def test_declared_observer_cannot_be_missing(self, _inputs):
        self.declare_power()
        self.windows()
        with self.assertRaises(ValueError):
            harness.analyze(self.cohort)
        self.assertFalse((self.cohort / 'report.json').exists())

    @patch.object(harness, 'harness_inputs', return_value=[])
    def test_declared_observer_links_every_window(self, _inputs):
        self.declare_power()
        self.windows()
        for label in ORDER:
            ref = self.store.load('windows/' + label)
            harness.seal(self.store, 'power_observers/' + label, 'power-observer',
                         {'passed': True, 'native_window_ref': ref, 'tool': {'sha256': 'fixed-tool'}}, [ref])
        self.assertTrue(harness.analyze(self.cohort)['passed'])

    @patch.object(harness, 'harness_inputs', return_value=[])
    def test_foreign_observer_does_not_qualify_window(self, _inputs):
        self.declare_power()
        self.windows()
        ref = self.store.load('windows/prime-A')
        harness.seal(self.store, 'power_observers/prime-A', 'power-observer',
                     {'passed': True, 'native_window_ref': ref, 'tool': {'sha256': 'foreign-tool'}}, [ref])
        with self.assertRaisesRegex(ValueError, 'foreign power observer'):
            harness.analyze(self.cohort)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.cohort = self.root / "cohort"
        self.cohort.mkdir()
        self.store = harness.store_open(self.cohort, create=True)
        self.addCleanup(self.store.close)
        self.addCleanup(self.tmp.cleanup)
        self.protocol = {"cohort_id": "test-uuid", "harness": [], "host": {}, "assets": [],
                         "spec": {"order": ORDER, "assets": [], "pci": "test", "driver_path": "test",
                                  "max_baseline_range_percent": 1.0, "warmup_seconds": 60, "window_seconds": 120}}
        self.protocol_ref = harness.seal(self.store, "protocol", "experiment-protocol", self.protocol)

    def windows(self, missing=None):
        child = self.store.put_record("quiet-boundary", {"passed": True})
        for label in ORDER:
            if label == missing:
                continue
            variant = "B" if label.startswith("B") or label == "prime-B" else "A"
            frames, seconds = (1000, 120) if variant == "A" else (1200, 120)
            payload = {"id": label, "variant": variant, "cohort_id": self.protocol["cohort_id"], "passed": True,
                       "signature": {"hardware": "fixed", "quality": "High"},
                       "statistics": {"measurement": {"frames": frames, "seconds": seconds,
                                                        "mean_ms": seconds * 1000 / frames, "fps": frames / seconds}}}
            harness.seal(self.store, f"windows/{label}", "native-window", payload, [child, self.protocol_ref])
        return child

    @patch.object(harness, "harness_inputs", return_value=[])
    def test_complete_analysis_has_recursive_closure_and_exclusive_report(self, _inputs):
        self.windows()
        result = harness.analyze(self.cohort)
        self.assertTrue(result["passed"])
        self.assertTrue(result["evidence_verified_recursively"])
        sealed = self.store.verify(self.store.load("analysis"))["payload"]
        self.assertEqual(result, sealed)
        self.assertEqual(json.loads((self.cohort / "report.json").read_text()), sealed)
        with self.assertRaises(FileExistsError):
            harness.analyze(self.cohort)

    @patch.object(harness, "harness_inputs", return_value=[])
    def test_monitored_analysis_retains_readings_outside_identity_and_rejects_missing_or_wrong_contract(self, _inputs):
        for error in (None, "missing_observations", "wrong_contract"):
            with self.subTest(error=error):
                cohort = self.root / ("monitored-" + str(error))
                cohort.mkdir()
                with harness.store_open(cohort, create=True) as store:
                    protocol = deepcopy(self.protocol)
                    protocol["spec"]["hardware_monitoring"] = monitoring_contract()
                    protocol_ref = harness.seal(store, "protocol", "experiment-protocol", protocol)
                    for i, label in enumerate(ORDER):
                        variant = "B" if label.startswith("B") or label == "prime-B" else "A"
                        frames = 1200 if variant == "B" else 1000
                        observations = sensor_observations()
                        observations["gpu_temperature_c"] += i
                        payload = {"id": label, "variant": variant, "cohort_id": protocol["cohort_id"], "passed": True,
                                   "signature": {"hardware_monitoring": monitoring_contract()},
                                   "hardware_monitoring_observations": observations,
                                   "statistics": {"measurement": {"frames": frames, "seconds": 120,
                                                                  "mean_ms": 120000 / frames, "fps": frames / 120}}}
                        if error == "missing_observations" and label == "A1":
                            del payload["hardware_monitoring_observations"]
                        if error == "wrong_contract" and label == "A1":
                            payload["signature"]["hardware_monitoring"]["mode"] = "disabled"
                        harness.seal(store, f"windows/{label}", "native-window", payload, [protocol_ref])
                    if error:
                        with self.assertRaises(ValueError):
                            harness.analyze(cohort)
                        self.assertFalse((cohort / "report.json").exists())
                    else:
                        result = harness.analyze(cohort)
                        self.assertTrue(result["passed"])
                        self.assertEqual(result["hardware_monitoring"]["contract"], monitoring_contract())
                        observed = result["hardware_monitoring"]["observations_by_window"]
                        self.assertEqual(len(observed), len(ORDER))
                        self.assertNotEqual(observed["A1"]["gpu_temperature_c"], observed["B1"]["gpu_temperature_c"])

    @patch.object(harness, "harness_inputs", return_value=[])
    def test_missing_window_never_produces_a_comparison(self, _inputs):
        self.windows(missing="A4")
        with self.assertRaises(ValueError):
            harness.analyze(self.cohort)
        self.assertFalse((self.cohort / "report.json").exists())

    @patch.object(harness, "harness_inputs", return_value=[])
    def test_nested_boundary_deletion_invalidates_parent_reports(self, _inputs):
        child = self.windows()
        harness.analyze(self.cohort)
        (self.store.root / "objects" / child["sha256"]).unlink()
        with self.assertRaises(ValueError):
            self.store.verify(self.store.load("analysis"))

    @patch.object(harness, "harness_inputs", return_value=[])
    def test_nested_boundary_corruption_blocks_analysis_before_metrics(self, _inputs):
        child = self.windows()
        path = self.store.root / "objects" / child["sha256"]
        path.chmod(0o600)
        path.write_bytes(b"corrupt")
        with patch.object(harness, "compare_abba") as compare:
            with self.assertRaises(ValueError):
                harness.analyze(self.cohort)
            compare.assert_not_called()

    @patch.object(harness, "harness_inputs", return_value=[{"sha256": "changed"}])
    def test_changed_analyzer_is_rejected(self, _inputs):
        self.windows()
        with self.assertRaisesRegex(ValueError, "analysis harness changed"):
            harness.analyze(self.cohort)

    @patch.object(harness, "host_identity", return_value={})
    @patch.object(harness, "harness_inputs", return_value=[])
    def test_existing_attempt_cannot_launch_or_modify_work(self, _inputs, _host):
        harness.seal(self.store, "attempts/prime-A", "attempt", {"id": "prime-A"}, [self.protocol_ref])
        with patch.object(harness, "run_owned") as native:
            with self.assertRaisesRegex(ValueError, "existing immutable run"):
                harness.run(self.cohort, "prime-A")
            native.assert_not_called()
        self.assertFalse((self.cohort / "work").exists())

    def test_tree_round_trip_is_private_writable_and_digest_preserving(self):
        source = self.root / "seed"
        (source / "nested").mkdir(parents=True)
        (source / "nested/cache").write_bytes(b"geometry")
        ref = harness.snapshot_tree(self.store, source)
        destination = self.root / "copy"
        harness.restore_tree(self.store, ref, destination)
        path = destination / "nested/cache"
        self.assertEqual(path.read_bytes(), b"geometry")
        path.write_bytes(b"changed")
        self.assertEqual((source / "nested/cache").read_bytes(), b"geometry")
        self.store.verify(ref)
        with self.assertRaises(FileExistsError):
            harness.restore_tree(self.store, ref, destination)

    def test_tree_symlink_is_rejected(self):
        source = self.root / "seed"
        source.mkdir()
        (source / "link").symlink_to(__file__)
        with self.assertRaises(ValueError):
            harness.snapshot_tree(self.store, source)

    def test_actual_owned_executable_and_mapped_library_with_negative_controls(self):
        binary = Path(shutil.which("sleep")).resolve()
        child = subprocess.Popen([str(binary), "5"])
        try:
            proc = Path(f"/proc/{child.pid}")
            library = None
            for _ in range(50):
                rows = (proc / "maps").read_text().splitlines()
                library = next((line.split(None, 5)[5] for line in rows if "/libc.so" in line), None)
                if library:
                    break
                time.sleep(.01)
            self.assertIsNotNone(library)
            start = int((proc / "stat").read_text().rsplit(")", 1)[1].split()[19])
            native = {"pid": child.pid, "start_ticks": start}
            identity = {**file_identity(library), "mapping": mapped_file_identity(library)}
            self.assertIsNotNone(harness.native_ownership(child.pid, native, binary, library, identity))
            wrong = deepcopy(identity)
            wrong["mapping"]["inode"] += 1
            self.assertIsNone(harness.native_ownership(child.pid, native, binary, library, wrong))
            wrong = deepcopy(identity)
            wrong["mapping"]["device_minor"] += 1
            self.assertIsNone(harness.native_ownership(child.pid, native, binary, library, wrong))
            self.assertIsNone(harness.native_ownership(child.pid, {**native, "start_ticks": start + 1}, binary, library, identity))
            self.assertIsNone(harness.native_ownership(child.pid, native, Path(sys.executable), library, identity))
        finally:
            child.terminate()
            child.wait(timeout=5)

    def test_geometry_requires_one_full_runtime_signature(self):
        text = """Loaded 64 unique meshes, 321 instances, 523108 vertices, 623503 triangles
Tessellated geometry: 10394693 vertices, 19355485 triangles (base: 523108 vertices, 623503 triangles)
Bounded displacement: 1 prepared surfaces, 393 fallbacks, 2967597 added unique triangles, 26708373 added rendered triangles (ceiling 27000000)
MassiveFoliage buffers: 6 source/category owners, 100 source ranges, 207 patches, 19104504 uploaded vertices / 39392475 instantiated vertices, 1146270240 bytes uploaded (1217278260 bytes saved by sharing)
CPU tessellation: 500 surfaces (500 from cache)
CPU tessellation (skinned): 1 surfaces (1 from cache)
"""
        self.assertEqual(harness.geometry(text)["base"], [64, 321, 523108, 623503])
        for altered in (text + text, text.replace("MassiveFoliage buffers", "missing")):
            with self.assertRaises(ValueError):
                harness.geometry(altered)


if __name__ == "__main__":
    unittest.main()
