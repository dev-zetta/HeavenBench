#!/usr/bin/env python3
"""Exercise visual-audit receipts without a display, renderer, or original assets."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("visual_audit", ROOT / "scripts/visual_audit.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def platform_os(name):
    # Only the runner's platform boundary is simulated. Changing os.name on
    # the real module would also change pathlib's host-path implementation.
    return SimpleNamespace(**{**vars(os), "name": name})


def write_pfm(path, values=None, byte_order="<", width=2, height=2):
    values = [0.5] * (width * height * 3) if values is None else values
    scale = "-1.0" if byte_order == "<" else "1.0"
    path.write_bytes(f"PF\n{width} {height}\n{scale}\n".encode() +
                     struct.pack(f"{byte_order}{len(values)}f", *values))


def write_capture(image, config, frames=3):
    image.parent.mkdir(parents=True, exist_ok=True)
    width, height = config["width"], config["height"]
    image.write_bytes(f"P6\n{width} {height}\n255\n".encode() + b"\x80" * (width * height * 3))
    metadata = {"render_api": "Vulkan", "benchmark_score": False,
                "presented_frames": frames, "width": width, "height": height,
                "simulation_time": frames / 60, "fixed_step": 1 / 60,
                "tour_time": None, "tour_motion": False, "camera_fov": 60,
                "floating_islands_frozen": True}
    if config.get("hdr", False):
        raw = image.with_suffix(".pq.pfm")
        write_pfm(raw, width=width, height=height)
        metadata["hdr_pq_image"] = str(raw)
    Path(str(image) + ".json").write_text(json.dumps(metadata))
    Path(str(image) + ".config.json").write_text(json.dumps(config))


class VisualAuditTest(unittest.TestCase):
    def test_procedural_manifest_covers_every_feature_with_isolated_pairs(self):
        cases = json.loads((ROOT / "tests/visual/render_test_scenes.json").read_text())
        for case in cases:
            AUDIT.validate_case(case)
            self.assertEqual(case["expected_metadata"]["render_test_scene"],
                             case["settings"]["render_test_scene"])
            self.assertTrue(case["expected_visible_behavior"])
            self.assertTrue(case["active_input_requirements"])
            self.assertNotIn("--test-scene", case["args"])
            self.assertEqual(case["fps"], 60)
        self.assertEqual(len({case["name"] for case in cases}), len(cases))

        # New engine fields or scene IDs must acquire an actual maintained
        # comparison instead of silently inheriting an unrelated capture.
        types = (ROOT / "benchmark/src/engine/types.h").read_text()
        feature_struct = types.split("struct FeatureToggles {", 1)[1].split("};", 1)[0]
        fields = set(re.findall(r"\bbool\s+(\w+)\s*=", feature_struct))
        catalog = (ROOT / "benchmark/src/engine/render_test_scene_catalog.cpp").read_text()
        scene_ids = set(re.findall(r'^\s*\{"([a-z0-9-]+)",', catalog, re.MULTILINE))
        self.assertEqual({case["settings"]["render_test_scene"] for case in cases}, scene_ids - AUDIT.AUTHORED_SCENES)
        self.assertEqual({case["settings"]["render_test_scene"] for case in cases}, AUDIT.PROCEDURAL_SCENES)
        documented = (ROOT / "docs/RENDER_TEST_SCENES.md").read_text()
        for field in fields:
            self.assertIn(f"| `{field}` |", documented)
        groups = {}
        for case in cases:
            comparison = case["comparison"]
            if comparison["kind"] == "feature_toggle":
                groups.setdefault((comparison["feature"], comparison.get("variant")), []).append(case)
        self.assertEqual({feature for feature, _variant in groups}, fields)
        cli_only = {"color_grading": "color-grading", "physical_camera": "physical-camera",
                    "dynamic_resolution": "dynamic-resolution"}
        serialized = {"features." + field for field in fields - set(cli_only) - {"heat_haze"}}
        serialized.add("heat_haze_enabled")
        for (field, variant), pair in groups.items():
            with self.subTest(field=field, variant=variant):
                self.assertEqual(len(pair), 2)
                by_state = {case["comparison"]["state"]: case for case in pair}
                self.assertEqual(set(by_state), {"on", "off"})
                on, off = by_state["on"], by_state["off"]
                for key in ("pose", "frames", "count", "fps", "active_input_requirements",
                            "expected_visible_behavior"):
                    self.assertEqual(on[key], off[key])
                self.assertTrue(serialized.issubset(on["settings"]))
                if field in cli_only:
                    self.assertEqual(on["settings"], off["settings"])
                    flag = cli_only[field]
                    self.assertEqual([f"--no-{flag}" if arg == f"--{flag}" else arg
                                      for arg in on["args"]], off["args"])
                    self.assertTrue(on["expected_metadata"][f"renderer_features.{field}"])
                    self.assertFalse(off["expected_metadata"][f"renderer_features.{field}"])
                else:
                    key = "heat_haze_enabled" if field == "heat_haze" else f"features.{field}"
                    self.assertIs(on["settings"][key], True)
                    self.assertIs(off["settings"][key], False)
                    self.assertEqual({**on["settings"], key: False}, off["settings"])
                    self.assertEqual(on["args"], off["args"])

        enabled = {field: next(case for case in pair if case["comparison"]["state"] == "on")
                   for (field, variant), pair in groups.items() if variant is None}
        self.assertTrue(enabled["heat_haze"]["settings"]["features.particles"])
        self.assertTrue(enabled["depth_of_field"]["expected_metadata"]["renderer_features.physical_camera"])
        self.assertEqual(enabled["depth_of_field"]["expected_metadata"]["runtime_settings.aperture"], 1.4)
        self.assertEqual(enabled["depth_of_field"]["expected_metadata"]["runtime_settings.shutter"], 0.000125)
        self.assertGreater(enabled["wet_reflections"]["settings"]["weather.rain_intensity"], 0)
        self.assertTrue(enabled["colored_penumbra"]["settings"]["features.soft_shadows"])
        self.assertEqual(enabled["colored_penumbra"]["settings"]["time_of_day"], 8)
        for feature, other in (("grass", "massive_foliage"), ("massive_foliage", "grass"),
                               ("clouds", "volumetric_clouds"), ("volumetric_clouds", "clouds")):
            self.assertFalse(enabled[feature]["settings"][f"features.{other}"])

    def test_displaced_contact_pairs_keep_height_inputs_and_isolate_only_contacts(self):
        cases = json.loads((ROOT / "tests/visual/render_test_scenes.json").read_text())
        selected = [c for c in cases if c["name"].startswith("test-displaced-contact-")]
        self.assertEqual({c["name"] for c in selected}, {
            f"test-displaced-contact-{hour:02d}-{state}" for hour in (8, 16) for state in ("on", "off")})
        for hour in (8, 16):
            pair = {c["comparison"]["state"]: c for c in selected if c["settings"]["time_of_day"] == hour}
            on, off = pair["on"], pair["off"]
            self.assertEqual({**on["settings"], "features.contact_shadows": False}, off["settings"])
            self.assertEqual(on["comparison"], {"kind": "feature_toggle", "feature": "contact_shadows",
                                               "variant": f"displaced-{hour:02d}", "state": "on"})
            for c in pair.values():
                self.assertEqual(c["settings"]["render_test_scene"], "displacement")
                self.assertTrue(c["settings"]["tessellation.enabled"])
                self.assertTrue(c["settings"]["features.ground_relief"])
                self.assertFalse(c["settings"]["features.soft_shadows"])
                self.assertFalse(c["settings"]["features.ambient_occlusion"])
                self.assertEqual(c["expected_metadata"]["time_of_day"], hour)
                for field in ("objects", "meshes", "material_records"):
                    self.assertEqual(c["expected_metadata"][f"render_test_inputs.{field}"], 5)
                self.assertEqual(c["expected_metadata"]["render_test_inputs.grounds"], 1)
                self.assertEqual((c["frames"], c["count"], c["fps"]), (120, 2, 60))
            for field in ("pose", "args", "expected_metadata", "active_input_requirements"):
                self.assertEqual(on[field], off[field])
        self.assertEqual(selected[0]["pose"], selected[2]["pose"])
        self.assertEqual({**selected[0]["settings"], "time_of_day": 16}, selected[2]["settings"])

    def test_pure_fixture_source_identity_never_requires_asset_inputs(self):
        missing_data = self.root / "nonexistent-test-assets"
        selected = [{"settings": {"render_test_scene": "displacement"}},
                    {"settings": {"render_test_scene": "contact-shadow"}}]
        with patch.object(AUDIT, "data_fingerprint", side_effect=AssertionError("asset input read")):
            identity = AUDIT.source_fingerprint(missing_data, selected)
        self.assertFalse(missing_data.exists())
        self.assertIs(identity["asset_data_used"], False)
        self.assertEqual(identity["scene_ids"], ["contact-shadow", "displacement"])
        sources = identity["current_source_files"]
        self.assertEqual(len(sources), 6)
        for relative, fingerprint in sources.items():
            source = ROOT / relative
            self.assertEqual(fingerprint, {"sha256": AUDIT.sha256(source), "size": source.stat().st_size})
        # A missing generator is not replaced with an asset-free success stamp.
        with patch.object(AUDIT, "ROOT", self.root), self.assertRaisesRegex(ValueError, "source is missing"):
            AUDIT.source_fingerprint(missing_data, selected)

    def test_mixed_and_original_selections_still_require_real_asset_identity(self):
        fixture = {"settings": {"render_test_scene": "displacement"}}
        for selected in ([{}], [{"settings": {"render_test_scene": ""}}], []):
            with self.subTest(selected=selected), patch.object(AUDIT, "data_fingerprint", return_value={"assets": True}) as data:
                self.assertEqual(AUDIT.source_fingerprint(self.root, selected), {"assets": True})
                data.assert_called_once_with(self.root)
        with self.assertRaisesRegex(ValueError, "does not exist"):
            AUDIT.source_fingerprint(self.root / "nonexistent-test-assets", [fixture, {}])

    def fixture_source_tree(self):
        tree = self.root / "source-tree"
        for name in AUDIT.PROCEDURAL_SOURCE_FILES + AUDIT.AUTHORED_SOURCE_FILES:
            source = tree / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(f"source bytes for {name}\n")
        return tree

    def test_airship_fixture_requires_actual_asset_and_node_provenance(self):
        tree = self.fixture_source_tree()
        data = self.root / "airship assets"
        node = data / "heaven/nodes/airship.node"
        node.parent.mkdir(parents=True)
        node.write_bytes(b"original airship node input")
        selected = {"name": "airship", "settings": {"render_test_scene": "asset-airship"},
                    "asset_source_nodes": ["heaven/nodes/airship.node"]}
        AUDIT.validate_case(selected)
        self.assertEqual(AUDIT.fixture_source_kind("asset-airship"), "authored-assets")
        identity = AUDIT.source_fingerprint(data, [selected], tree)
        self.assertIs(identity["asset_data_used"], True)
        self.assertEqual(identity["scene_ids"], ["asset-airship"])
        self.assertIn("benchmark/src/engine/scene_rhi.cpp", identity["current_source_files"])
        AUDIT.verify_source_fingerprint(identity, selected, source_root=tree)
        metadata = {"render_test_scene": "asset-airship", "render_test_source_kind": "authored-assets",
                    "render_test_asset_sources": ["heaven/nodes/airship.node", "heaven/nodes/generator.node"]}
        AUDIT.verify_fixture_metadata(metadata, selected)
        with self.assertRaisesRegex(ValueError, "does not exist"):
            AUDIT.source_fingerprint(self.root / "missing airship assets", [selected], tree)
        node.write_bytes(b"changed airship input")
        with self.assertRaisesRegex(ValueError, "asset data changed"):
            AUDIT.verify_source_fingerprint(identity, selected, source_root=tree)
        with self.assertRaises(ValueError):
            AUDIT.verify_fixture_metadata({**metadata, "render_test_asset_sources": ["heaven/nodes/generator.node"]}, selected)

    def test_authored_and_mixed_sources_require_data_and_loader_catalog_hashes(self):
        tree = self.fixture_source_tree()
        data = self.root / "actual assets"
        (data / "heaven/nodes").mkdir(parents=True)
        node = data / "heaven/nodes/house2.node"
        node.write_bytes(b"authored binary node bytes")
        authored = {"settings": {"render_test_scene": "asset-contact-shadow"}}
        procedural = {"settings": {"render_test_scene": "displacement"}}
        with patch.object(AUDIT, "ROOT", tree):
            pure = AUDIT.source_fingerprint(data, [authored])
            self.assertEqual(pure["kind"], "authored-asset-render-test-scenes")
            self.assertIs(pure["asset_data_used"], True)
            self.assertFalse(pure["includes_original_scene"])
            self.assertEqual(pure["asset_data"], AUDIT.data_fingerprint(data))
            self.assertIsNone(pure["sibling_core_data"])
            self.assertEqual(set(pure["current_source_files"]), set(AUDIT.PROCEDURAL_SOURCE_FILES + AUDIT.AUTHORED_SOURCE_FILES))
            for selected in ([procedural, authored], [procedural, {}]):
                mixed = AUDIT.source_fingerprint(data, selected)
                self.assertEqual(mixed["kind"], "mixed-render-test-scenes")
                self.assertTrue(mixed["asset_data_used"])
                for c in selected:
                    if c.get("settings"):
                        AUDIT.verify_source_fingerprint(mixed, c)
            missing = self.root / "missing"
            with self.assertRaisesRegex(ValueError, "does not exist"):
                AUDIT.source_fingerprint(missing, [authored])
            empty = self.root / "empty"; empty.mkdir()
            with self.assertRaisesRegex(ValueError, "empty"):
                AUDIT.source_fingerprint(empty, [authored])
            empty_file = self.root / "empty.zip"; empty_file.touch()
            with self.assertRaisesRegex(ValueError, "empty"):
                AUDIT.source_fingerprint(empty_file, [authored])
            AUDIT.verify_source_fingerprint(pure, authored)
            node.write_bytes(b"changed actual node")
            changed = AUDIT.source_fingerprint(data, [authored])
            self.assertNotEqual(pure["asset_data"], changed["asset_data"])
            with self.assertRaisesRegex(ValueError, "asset data changed"):
                AUDIT.verify_source_fingerprint(pure, authored)
            loader = tree / AUDIT.AUTHORED_SOURCE_FILES[0]
            loader.write_text("changed fixture loader\n")
            with self.assertRaisesRegex(ValueError, "source changed"):
                AUDIT.verify_source_fingerprint(changed, authored)
            loader.unlink()
            with self.assertRaisesRegex(ValueError, "source is missing"):
                AUDIT.source_fingerprint(data, [authored])

    def test_authored_loose_assets_include_sibling_core_and_packed_inputs_keep_file_identity(self):
        tree = self.fixture_source_tree()
        data = self.root / "extracted/heaven"
        data.mkdir(parents=True)
        (data / "house.node").write_bytes(b"node")
        core = data.parent / "core/core/materials"
        core.mkdir(parents=True)
        material = core / "mesh_base.mat"
        material.write_bytes(b"original base material inheritance")
        authored = {"settings": {"render_test_scene": "asset-foliage"}}
        with patch.object(AUDIT, "ROOT", tree):
            original = AUDIT.source_fingerprint(data, [authored])
            self.assertEqual(original["sibling_core_data"], AUDIT.data_fingerprint(data.parent / "core"))
            AUDIT.verify_source_fingerprint(original, authored)
            material.write_bytes(b"changed core material inheritance")
            with self.assertRaisesRegex(ValueError, "sibling core data changed"):
                AUDIT.verify_source_fingerprint(original, authored)
            changed = AUDIT.source_fingerprint(data, [authored])
            material.unlink()
            with self.assertRaisesRegex(ValueError, "sibling core data changed"):
                AUDIT.verify_source_fingerprint(changed, authored)
            package = self.root / "bundle.zip"
            package.write_bytes(b"complete asset package bytes")
            packed = AUDIT.source_fingerprint(package, [authored])
            self.assertEqual(packed["asset_data"], {"path": str(package), "sha256": AUDIT.sha256(package)})
            self.assertIsNone(packed["sibling_core_data"])
            AUDIT.verify_source_fingerprint(packed, authored)

    def test_source_snapshot_keeps_hash_checks_and_actual_asset_paths(self):
        live = self.fixture_source_tree()
        snapshot = self.root / "frozen-source"
        shutil.copytree(live, snapshot)
        data = self.root / "assets"
        data.mkdir()
        node = data / "house.node"
        node.write_bytes(b"actual node")
        core = data.parent / "core"
        core.mkdir()
        material = core / "base.mat"
        material.write_bytes(b"actual material")
        authored = {"settings": {"render_test_scene": "asset-contact-shadow"}}
        with patch.object(AUDIT, "ROOT", live):
            receipt = AUDIT.source_fingerprint(data, [authored])
            relative = AUDIT.AUTHORED_SOURCE_FILES[0]
            loader = live / relative
            loader.write_text("live drift\n")
            with self.assertRaisesRegex(ValueError, "source changed"):
                AUDIT.verify_source_fingerprint(receipt, authored)
            cache = {}
            replay = AUDIT.verify_source_fingerprint(receipt, authored, cache, snapshot)
            self.assertEqual(replay["source_root"], str(snapshot.resolve()))
            self.assertEqual(replay["source_scope"], "declared-snapshot")
            # A successful snapshot cache entry cannot authorize the live tree.
            with self.assertRaisesRegex(ValueError, "source changed"):
                AUDIT.verify_source_fingerprint(receipt, authored, cache)
            frozen_loader = snapshot / relative
            original = frozen_loader.read_bytes()
            frozen_loader.write_bytes(b"tampered snapshot")
            with self.assertRaisesRegex(ValueError, "source changed"):
                AUDIT.verify_source_fingerprint(receipt, authored, source_root=snapshot)
            frozen_loader.write_bytes(original)
            declared = AUDIT.source_fingerprint(data, [authored], snapshot)
            self.assertEqual(declared["source_root"], str(snapshot.resolve()))
            self.assertEqual(declared["current_source_files"], receipt["current_source_files"])
            self.assertEqual(declared["asset_data"], receipt["asset_data"])
            node.write_bytes(b"changed actual node")
            with self.assertRaisesRegex(ValueError, "asset data changed"):
                AUDIT.verify_source_fingerprint(receipt, authored, source_root=snapshot)
            node.write_bytes(b"actual node")
            material.write_bytes(b"changed actual material")
            with self.assertRaisesRegex(ValueError, "sibling core data changed"):
                AUDIT.verify_source_fingerprint(receipt, authored, source_root=snapshot)
            with self.assertRaisesRegex(ValueError, "not a directory"):
                AUDIT.verify_source_fingerprint(receipt, authored, source_root=self.root / "absent")

    def test_explicit_procedural_snapshot_requires_recorded_generator_bytes(self):
        live = self.fixture_source_tree()
        scene = {"settings": {"render_test_scene": "displacement"}}
        with patch.object(AUDIT, "ROOT", live):
            receipt = AUDIT.source_fingerprint(self.root / "absent-data", [scene], live)
            replay = AUDIT.verify_source_fingerprint(receipt, scene, source_root=live)
            self.assertTrue(replay["source_fingerprint_replayed"])
            source = live / AUDIT.PROCEDURAL_SOURCE_FILES[0]
            source.write_text("changed procedural generator")
            with self.assertRaisesRegex(ValueError, "source changed"):
                AUDIT.verify_source_fingerprint(receipt, scene, source_root=live)
            legacy = {"kind": "procedural-render-test-scenes", "asset_data_used": False}
            AUDIT.verify_source_fingerprint(legacy, scene)
            with self.assertRaisesRegex(ValueError, "identities are incomplete"):
                AUDIT.verify_source_fingerprint(legacy, scene, source_root=live)

    def test_unknown_fixture_cannot_be_stamped_asset_free(self):
        unknown = {"name": "unknown", "settings": {"render_test_scene": "asset-unknown"}}
        for action in (lambda: AUDIT.validate_case(unknown),
                       lambda: AUDIT.source_fingerprint(self.root / "absent", [unknown])):
            with self.assertRaisesRegex(ValueError, "Unknown render-test scene"):
                action()
        authored = {"settings": {"render_test_scene": "asset-animation"}}
        with self.assertRaisesRegex(ValueError, "asset-independent"):
            AUDIT.verify_source_fingerprint({"kind": "procedural-render-test-scenes", "asset_data_used": False}, authored)

    def test_authored_metadata_requires_source_kind_and_every_selected_node(self):
        authored = {"name": "asset", "settings": {"render_test_scene": "asset-contact-shadow"},
                    "asset_source_nodes": ["heaven/nodes/house2.node"]}
        AUDIT.validate_case(authored)
        metadata = {"render_test_scene": "asset-contact-shadow", "render_test_source_kind": "authored-assets",
                    "render_test_asset_sources": ["heaven/nodes/house2.node", "heaven/nodes/nested.node"]}
        AUDIT.verify_fixture_metadata(metadata, authored)
        for mutation in ({"render_test_source_kind": "procedural"}, {"render_test_asset_sources": []},
                         {"render_test_asset_sources": ["../secret.node"]},
                         {"render_test_asset_sources": ["heaven/nodes/nested.node"]}):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                AUDIT.verify_fixture_metadata({**metadata, **mutation}, authored)
        procedural = {"name": "proc", "settings": {"render_test_scene": "displacement"}}
        AUDIT.verify_fixture_metadata({}, procedural)
        with self.assertRaisesRegex(ValueError, "authored source nodes"):
            AUDIT.verify_fixture_metadata({"render_test_asset_sources": ["heaven/nodes/house2.node"]}, procedural)
        with self.assertRaisesRegex(ValueError, "Only authored"):
            AUDIT.validate_case({**procedural, "asset_source_nodes": authored["asset_source_nodes"]})

    def test_procedural_scene_ids_reject_invalid_types_and_unsafe_names(self):
        for value in (True, 3, [], {}, "Bad_ID", "../scene", "contact shadow"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "procedural scene ID"):
                AUDIT.validate_case({"name": "bad-scene", "settings": {"render_test_scene": value}})
        for value in ("", "contact-shadow"):
            AUDIT.validate_case({"name": "scene", "settings": {"render_test_scene": value}})

    def test_sequence_requires_every_frame_and_correct_frame_numbers(self):
        case = {"name": "sequence", "count": 3}
        for index in range(3):
            image = self.image if index == 0 else self.image.with_name(
                f"{self.image.stem}.{index:06d}{self.image.suffix}")
            write_capture(image, self.config, frames=3 + index)
            metadata_path = Path(str(image) + ".json")
            metadata = json.loads(metadata_path.read_text())
            metadata.update(sequence_index=index, sequence_count=3)
            metadata_path.write_text(json.dumps(metadata))
        products = AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        self.assertEqual(len(products["sequence"]), 3)
        metadata["presented_frames"] = 4
        metadata_path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "frame/benchmark"):
            AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        metadata["presented_frames"] = 5
        metadata["sequence_index"] = 1
        metadata_path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "sequence metadata"):
            AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        metadata["sequence_index"] = 2
        metadata_path.write_text(json.dumps(metadata))
        image.unlink()
        with self.assertRaises(OSError):
            AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)

    def test_sequence_count_is_bounded_and_owned_by_manifest(self):
        for count in (0, -1, 601, True, 1.5, "16"):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, "sequence count"):
                AUDIT.validate_case({"name": "sequence", "count": count})
        for count in (1, 600):
            AUDIT.validate_case({"name": "sequence", "count": count})
        with self.assertRaisesRegex(ValueError, "controlled startup action"):
            AUDIT.validate_case({"name": "sequence", "args": ["--capture-count", "16"]})

    def test_capture_clock_modes_are_verified(self):
        write_capture(self.image, self.config)
        path = Path(str(self.image) + ".json")
        metadata = json.loads(path.read_text())
        case = {"name": "slow", "fps": 15}
        with self.assertRaisesRegex(ValueError, "simulation_time"):
            AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        metadata.update(simulation_time=0.2, fixed_step=1 / 15)
        path.write_text(json.dumps(metadata))
        AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        case = {"name": "live", "realtime": True}
        with self.assertRaisesRegex(ValueError, "real-time clocks"):
            AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        metadata.update(simulation_time=0.23, fixed_step=None, realtime=True)
        path.write_text(json.dumps(metadata))
        AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        for case in ({"name": "bad", "fps": 0}, {"name": "bad", "fps": True},
                     {"name": "bad", "realtime": 1}, {"name": "bad", "realtime": True, "fps": 15}):
            with self.assertRaises(ValueError):
                AUDIT.validate_case(case)

    def test_realtime_sequence_rejects_stalled_time(self):
        case = {"name": "live", "realtime": True, "count": 2}
        for index in range(2):
            image = self.image if index == 0 else self.image.with_name(
                f"{self.image.stem}.{index:06d}{self.image.suffix}")
            write_capture(image, self.config, frames=3 + index)
            path = Path(str(image) + ".json")
            metadata = json.loads(path.read_text())
            metadata.update(sequence_index=index, sequence_count=2,
                            simulation_time=0.2, fixed_step=None, realtime=True)
            path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "time did not advance"):
            AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)
        metadata["simulation_time"] = 0.3
        path.write_text(json.dumps(metadata))
        AUDIT.verify_sequence(self.image, self.config, "vulkan", 3, case)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven visual audit ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.image = self.root / "capture with spaces.ppm"
        self.raw = self.image.with_suffix(".pq.pfm")
        self.config = {"width": 2, "height": 2, "hdr": True}

    def test_runtime_metadata_rejects_ignored_cli_controls(self):
        write_capture(self.image, self.config)
        metadata_path = Path(str(self.image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        expected = {"renderer_features.dynamic_resolution": True,
                    "runtime_settings.dynamic_resolution_min_scale": 0.5}
        with self.assertRaisesRegex(ValueError, "did not apply"):
            AUDIT.verify_capture(self.image, self.config, "vulkan", 3, expected)
        metadata.update({"renderer_features": {"dynamic_resolution": True},
                         "runtime_settings": {"dynamic_resolution_min_scale": 0.5}})
        metadata_path.write_text(json.dumps(metadata))
        AUDIT.verify_capture(self.image, self.config, "vulkan", 3, expected)
        metadata["renderer_features"]["dynamic_resolution"] = 1
        metadata_path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "did not apply"):
            AUDIT.verify_capture(self.image, self.config, "vulkan", 3, expected)

    def test_manifest_cannot_rebuild_or_override_the_launcher(self):
        for argument in ("--build", "-b", "--build-dir", "--compiler", "--cpu-arch",
                         "--gpu", "--list-gpus", "--wayland", "--x11", "--help", "-h",
                         "--test-scene", "--list-test-scenes"):
            with self.subTest(argument=argument), self.assertRaisesRegex(ValueError, "controlled startup action"):
                AUDIT.validate_case({"name": "wrapper-override", "args": [argument]})
        AUDIT.validate_case({"name": "renderer-options", "args": ["--camera-fov", "30", "--no-bloom"]})

    def test_pose_and_hour_must_match_the_actual_capture(self):
        case = {"name": "window", "pose": [-16.55995, 28.88356, 22.25873, -15.58437, 0],
                "args": ["--camera-fov", "45", "--camera-fov", "30"]}
        config = {**self.config, "time_of_day": 21}
        write_capture(self.image, config)
        metadata_path = Path(str(self.image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        metadata.update({"camera_position": case["pose"][:3], "camera_yaw": case["pose"][3],
                         "camera_pitch": 0, "camera_fov": 30, "time_of_day": 21})
        metadata_path.write_text(json.dumps(metadata))
        AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)
        for key, invalid in (("camera_position", [0, 0, 0]), ("camera_position", [True, 28.88356, 22.25873]),
                             ("camera_yaw", 0), ("camera_pitch", 1), ("camera_fov", 60),
                             ("time_of_day", 12), ("simulation_time", 1), ("fixed_step", 1),
                             ("presented_frames", 4), ("tour_time", 0), ("tour_motion", True)):
            with self.subTest(key=key, invalid=invalid):
                metadata_path.write_text(json.dumps({**metadata, key: invalid}))
                with self.assertRaises(ValueError):
                    AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)
        metadata_path.write_text(json.dumps(metadata))
        Path(str(self.image) + ".config.json").write_text(json.dumps({**config, "time_of_day": 12}))
        with self.assertRaisesRegex(ValueError, "Effective configuration did not apply time_of_day"):
            AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)

    def test_held_and_moving_tour_times_include_end_clamping(self):
        config = {**self.config, "time_of_day": 12}
        write_capture(self.image, config)
        metadata_path = Path(str(self.image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        # Authored tracks legitimately replace the configured hour and FOV.
        Path(str(self.image) + ".config.json").write_text(json.dumps({**config, "time_of_day": 21}))
        for moving, start, duration, expected in ((False, 18, 258.45, 18),
                                                (True, 18, 258.45, 18.05),
                                                (True, 258.43, 258.45, 258.45)):
            case = {"name": "tour", "tour_time": start,
                    "args": ["--capture-tour-motion"] if moving else []}
            valid = {**metadata, "tour_start_time": start, "tour_time": expected,
                     "tour_duration": duration, "tour_motion": moving, "time_of_day": 21,
                     "camera_fov": 75}
            metadata_path.write_text(json.dumps(valid))
            AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)
            for key, invalid in (("tour_time", expected + 1), ("tour_start_time", start + 1),
                                 ("tour_motion", not moving), ("tour_duration", None),
                                 ("tour_duration", -1)):
                with self.subTest(moving=moving, start=start, key=key):
                    metadata_path.write_text(json.dumps({**valid, key: invalid}))
                    with self.assertRaises(ValueError):
                        AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)
            case["args"] += ["--camera-fov", "30"]
            metadata_path.write_text(json.dumps(valid))
            with self.assertRaisesRegex(ValueError, "camera_fov"):
                AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)
            metadata_path.write_text(json.dumps({**valid, "camera_fov": 30.000001}))
            AUDIT.verify_capture(self.image, config, "vulkan", 3, case=case)

    def test_manifest_rejects_invalid_camera_fov_before_launch(self):
        for arguments in (["--camera-fov"], ["--camera-fov", "nan"],
                          ["--camera-fov", "inf"], ["--camera-fov", "0"],
                          ["--camera-fov", "171"], ["--camera-fov", "invalid"]):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(ValueError, "camera FOV"):
                AUDIT.validate_case({"name": "bad-fov", "args": arguments})

    def test_hdr_hash_covers_raw_bytes_in_both_byte_orders(self):
        write_capture(self.image, self.config)
        for order in ("<", ">"):
            with self.subTest(order=order):
                write_pfm(self.raw, [0.0, 0.5, 1.0] * 4, order)
                products = AUDIT.verify_capture(self.image, self.config, "vulkan", 3)
                self.assertEqual(products["hdr_pq_sha256"], AUDIT.sha256(self.raw))
        original = products["hdr_pq_sha256"]
        write_pfm(self.raw, [0.25] * 12, ">")
        self.assertNotEqual(AUDIT.verify_capture(self.image, self.config, "vulkan", 3)["hdr_pq_sha256"], original)

    def test_hdr_requires_raw_file_and_correct_metadata_path(self):
        write_capture(self.image, self.config)
        metadata_path = Path(str(self.image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        for declared in (None, "", str(self.root / "unrelated.pfm"), 123):
            with self.subTest(declared=declared):
                metadata["hdr_pq_image"] = declared
                metadata_path.write_text(json.dumps(metadata))
                with self.assertRaises(ValueError):
                    AUDIT.verify_capture(self.image, self.config, "vulkan", 3)
        write_capture(self.image, self.config)
        self.raw.unlink()
        with self.assertRaises(OSError):
            AUDIT.verify_capture(self.image, self.config, "vulkan", 3)

    def test_hdr_rejects_truncated_extra_and_invalid_headers(self):
        write_capture(self.image, self.config)
        valid = self.raw.read_bytes()
        invalid = (valid[:-1], valid + b"\x00", valid.replace(b"PF\n", b"Pf\n", 1),
                   valid.replace(b"2 2\n", b"3 2\n", 1),
                   valid.replace(b"-1.0\n", b"-2.0\n", 1),
                   valid.replace(b"-1.0\n", b"nan\n", 1))
        for data in invalid:
            with self.subTest(header=data[:20], size=len(data)):
                self.raw.write_bytes(data)
                with self.assertRaises(ValueError):
                    AUDIT.verify_capture(self.image, self.config, "vulkan", 3)

    def test_hdr_rejects_nonfinite_and_out_of_range_samples(self):
        write_capture(self.image, self.config)
        for value in (float("nan"), float("inf"), -float("inf"), -0.001, 1.001):
            for order in ("<", ">"):
                with self.subTest(value=value, order=order):
                    write_pfm(self.raw, [value] + [0.5] * 11, order)
                    with self.assertRaisesRegex(ValueError, "invalid PQ"):
                        AUDIT.verify_capture(self.image, self.config, "vulkan", 3)

    def test_sdr_does_not_require_hdr_and_rejects_conflicting_metadata(self):
        self.config["hdr"] = False
        write_capture(self.image, self.config)
        products = AUDIT.verify_capture(self.image, self.config, "vulkan", 3)
        self.assertNotIn("hdr_pq_sha256", products)
        metadata_path = Path(str(self.image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        metadata["hdr_pq_image"] = str(self.raw)
        metadata_path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "SDR capture"):
            AUDIT.verify_capture(self.image, self.config, "vulkan", 3)

    def test_feature_pairs_target_window_and_boiler_with_only_one_toggle(self):
        cases = {case["name"]: case for case in AUDIT.cases("features")}
        for feature in ("bloom", "particles", "heat_haze"):
            on, off = cases[f"{feature}-on"], cases[f"{feature}-off"]
            key = "heat_haze_enabled" if feature == "heat_haze" else f"features.{feature}"
            self.assertTrue(on["settings"][key])
            self.assertFalse(off["settings"][key])
            self.assertEqual({**on, "name": off["name"], "settings": {**on["settings"], key: False}}, off)
            AUDIT.validate_case(on)
            AUDIT.validate_case(off)
            self.assertNotIn("tour_time", on)
            if feature == "bloom":
                self.assertEqual(on["settings"]["time_of_day"], 21)
                self.assertEqual(on["args"], ["--camera-fov", "30"])
                self.assertEqual(on["pose"], [-16.55995, 28.88356, 22.25873, -15.58437, 0])
            elif feature == "heat_haze":
                self.assertEqual(on["settings"]["time_of_day"], 12)
                self.assertEqual(on["frames"], 240)
                self.assertEqual(on["pose"], [-53.5, 59.5, 17.5, -45, 0])
                self.assertEqual(on["args"], ["--camera-fov", "40"])
                self.assertTrue(on["settings"]["features.particles"])
            else:
                self.assertEqual(on["settings"]["time_of_day"], 12)
                self.assertEqual(on["frames"], 240)
                self.assertEqual(on["pose"], [-62, 62, 20, -30, -20])

    def test_default_matrix_uses_visible_receivers_and_preserves_isolated_pairs(self):
        all_cases = AUDIT.cases("all")
        self.assertEqual(len(all_cases), 135)
        self.assertEqual(len({case["name"] for case in all_cases}), 135)
        for case in all_cases:
            AUDIT.validate_case(case)
        cases = {case["name"]: case for case in all_cases}
        targets = {
            "god_rays": [4.23908, -18.56104, 24.2079, 34.37747, 30],
            "ground_relief": [-8, -17.5, 23.75, 90, -14],
            "clouds": [0, -45, 60, 0, -35],
            "volumetric_clouds": [0, -45, 60, 0, -35],
            "lens_flares": [-50.11721, 46.85126, 13.15872, 180, 0],
            "clear_coat": [15, -0.5, 31.3, -149.68879, -10.42822],
            "anisotropic": [-133.3, -89.5, 22, -135.78752, -19.28369],
            "cloth": [-12.47, -4.23, 37, -163.03707, -0.01527],
            "transparent_shadows": [7.2, -59.5, 16.6, 38.65981, -27.50055],
            "contact_shadows": [-133.3, -89.5, 22, -135.78752, -23.035],
            "point_light_shadows": [10, 3, 29.5, 140.85601, -11.22174],
            "refraction": [7.15, 5.9, 31.5, 136.58299, -7.05664],
            "ssgi": [-56, 44, 12.5, 59.03624, -16.25392],
        }
        for feature, pose in targets.items():
            with self.subTest(feature=feature):
                on, off = cases[f"{feature}-on"], cases[f"{feature}-off"]
                key = f"features.{feature}"
                self.assertEqual(on["pose"], pose)
                self.assertNotIn("tour_time", on)
                self.assertTrue(on["settings"][key])
                self.assertFalse(off["settings"][key])
                self.assertEqual({**on, "name": off["name"],
                                  "settings": {**on["settings"], key: False}}, off)
        self.assertFalse(cases["clouds-on"]["settings"]["features.volumetric_clouds"])
        self.assertFalse(cases["volumetric_clouds-on"]["settings"]["features.clouds"])
        self.assertFalse(cases["volumetric_clouds-on"]["settings"]["features.volumetric_fog"])
        self.assertEqual(cases["god_rays-on"]["settings"]["time_of_day"], 8)
        for feature in ("ground_relief", "transparent_shadows"):
            self.assertFalse(cases[f"{feature}-on"]["settings"]["features.grass"])
            self.assertFalse(cases[f"{feature}-on"]["settings"]["features.massive_foliage"])
        self.assertFalse(cases["contact_shadows-on"]["settings"]["features.ambient_occlusion"])
        self.assertEqual(cases["contact_shadows-on"]["settings"]["quality.contact_shadow_level"], 2)
        self.assertEqual(cases["point_light_shadows-on"]["settings"]["quality.shadow_light_budget"], 16)
        for feature in ("point_light_shadows", "refraction", "lens_flares"):
            self.assertFalse(cases[f"{feature}-on"]["settings"]["features.bloom"])
        self.assertFalse(cases["ssgi-on"]["settings"]["heat_haze_enabled"])
        self.assertEqual(cases["ssgi-on"]["settings"]["quality.ssgi_quality"], 2)
        self.assertEqual(cases["ssgi-on"]["frames"], 180)
        wind_on, wind_off = cases["wind-debug-on"], cases["wind-debug-off"]
        self.assertEqual(wind_on["pose"], targets["cloth"])
        self.assertEqual(wind_on["frames"], 180)
        self.assertEqual(wind_on["settings"], {"weather.rain_intensity": 0.0,
                                              "time_of_day": 9, "features.cloth": True})
        self.assertEqual(wind_off["args"], ["--camera-fov", "55"])
        self.assertEqual({**wind_off, "name": wind_on["name"],
                          "args": wind_off["args"] + ["--wind-debug"]}, wind_on)

    def run_audit(self, render, resume=False, case=None):
        manifest = self.root / "cases.json"
        manifest.write_text(json.dumps([case or {"name": "hdr", "settings": {"hdr": True}}]))
        binary = self.root / "unused renderer"
        binary.write_bytes(b"renderer fingerprint fixture")
        data = self.root / "unused assets.zip"
        data.write_bytes(b"asset fingerprint fixture")
        args = ["visual_audit.py", "--manifest", str(manifest), "--binary", str(binary),
                "--data", str(data), "--output", str(self.root / "output"),
                "--width", "2", "--height", "2", "--frames", "3"]
        if resume:
            args.append("--resume")
        with patch.object(sys, "argv", args), patch.object(AUDIT, "runtime_fingerprint", return_value={}), \
                patch.object(AUDIT, "os", platform_os("posix")), \
                patch.object(AUDIT, "run_capture", side_effect=render) as capture, \
                contextlib.redirect_stdout(io.StringIO()):
            result = AUDIT.main()
        return result, capture.call_count

    def test_non_posix_capture_rejected_without_changing_host_paths(self):
        native_name = os.name
        error = io.StringIO()
        with patch.object(AUDIT, "os", platform_os("nt")), \
                patch.object(sys, "argv", ["visual_audit.py", "--suite", "tour"]), \
                patch.object(AUDIT, "run_capture") as capture, \
                contextlib.redirect_stderr(error):
            self.assertEqual(os.name, native_name)
            self.assertEqual(Path(str(self.root)), self.root)
            with self.assertRaises(SystemExit) as rejected:
                AUDIT.main()
        self.assertEqual(rejected.exception.code, 2)
        self.assertIn("Linux run.sh and process-group interface", error.getvalue())
        capture.assert_not_called()

    def test_non_posix_can_list_cases_without_running_renderer(self):
        output = io.StringIO()
        with patch.object(AUDIT, "os", platform_os("nt")), \
                patch.object(sys, "argv", ["visual_audit.py", "--suite", "tour", "--list"]), \
                patch.object(AUDIT, "run_capture") as capture, \
                contextlib.redirect_stdout(output):
            self.assertEqual(AUDIT.main(), 0)
        listed = json.loads(output.getvalue())
        self.assertEqual([case["name"] for case in listed],
                         [case["name"] for case in AUDIT.cases("tour")])
        capture.assert_not_called()

    @staticmethod
    def render_ok(command, _env, log, _timeout):
        image = Path(command[command.index("--capture") + 1])
        config = json.loads(Path(command[command.index("--config") + 1]).read_text())
        write_capture(image, config)
        log.write("synthetic successful capture\n")
        return 0

    def test_resume_reuses_only_complete_unchanged_hdr_products(self):
        self.assertEqual(self.run_audit(self.render_ok), (0, 1))
        self.assertEqual(self.run_audit(self.render_ok, resume=True), (0, 0))
        raw = self.root / "output/hdr.pq.pfm"
        for mutation in (lambda: raw.unlink(), lambda: raw.write_bytes(b"truncated"),
                         lambda: write_pfm(raw, [0.25] * 12)):
            mutation()
            self.assertEqual(self.run_audit(self.render_ok, resume=True), (0, 1))
            self.assertEqual(self.run_audit(self.render_ok, resume=True), (0, 0))

    def test_orchestration_rejects_wrong_camera_even_after_successful_exit(self):
        case = {"name": "pose", "pose": [1, 2, 3, 4, 5],
                "settings": {"hdr": True, "time_of_day": 21}}

        def render(command, env, log, timeout):
            self.render_ok(command, env, log, timeout)
            image = Path(command[command.index("--capture") + 1])
            metadata_path = Path(str(image) + ".json")
            metadata = json.loads(metadata_path.read_text())
            metadata.update({"time_of_day": 21, "camera_yaw": 4, "camera_pitch": 5,
                             "camera_position": [1, 2, 3] if correct else [0, 0, 0]})
            metadata_path.write_text(json.dumps(metadata))
            return 0

        correct = False
        self.assertEqual(self.run_audit(render, case=case), (1, 1))
        receipt = json.loads((self.root / "output/results.json").read_text())[0]
        self.assertEqual(receipt["exit_code"], 0)
        self.assertFalse(receipt["success"])
        self.assertIn("camera position", receipt["error"])
        correct = True
        self.assertEqual(self.run_audit(render, resume=True, case=case), (0, 1))
        self.assertEqual(self.run_audit(render, resume=True, case=case), (0, 0))

    def test_legacy_products_can_be_checked_without_camera_case(self):
        write_capture(self.image, self.config)
        metadata_path = Path(str(self.image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        metadata.update({"tour_start_time": 18, "tour_time": 18, "tour_motion": False})
        self.assertNotIn("tour_duration", metadata)
        metadata_path.write_text(json.dumps(metadata))
        AUDIT.verify_capture(self.image, self.config, "vulkan", 3)

    def test_old_receipt_without_raw_hash_cannot_resume(self):
        self.assertEqual(self.run_audit(self.render_ok), (0, 1))
        path = self.root / "output/results.json"
        receipts = json.loads(path.read_text())
        del receipts[0]["hdr_pq_sha256"]
        path.write_text(json.dumps(receipts))
        self.assertEqual(self.run_audit(self.render_ok, resume=True), (0, 1))

    def test_failed_rerun_removes_stale_preview_sidecars_and_raw_output(self):
        self.assertEqual(self.run_audit(self.render_ok), (0, 1))
        image = self.root / "output/hdr.ppm"
        unrelated = self.root / "unrelated.pfm"
        unrelated.write_bytes(b"preserve this file")
        metadata_path = Path(str(image) + ".json")
        metadata = json.loads(metadata_path.read_text())
        metadata["hdr_pq_image"] = str(unrelated)
        metadata_path.write_text(json.dumps(metadata))

        def fail(_command, _env, log, _timeout):
            self.assertTrue(all(not path.exists() for path in AUDIT.capture_outputs(image)))
            log.write("synthetic failed capture\n")
            return 7

        self.assertEqual(self.run_audit(fail, resume=True), (1, 1))
        self.assertTrue(unrelated.exists())
        self.assertTrue(all(not path.exists() for path in AUDIT.capture_outputs(image)))
        receipt = json.loads((self.root / "output/results.json").read_text())[0]
        self.assertEqual(receipt["exit_code"], 7)
        self.assertFalse(receipt["success"])
        self.assertNotIn("hdr_pq_sha256", receipt)


if __name__ == "__main__":
    unittest.main()
