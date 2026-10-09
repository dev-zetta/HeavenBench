#!/usr/bin/env python3
"""Exercise render-scene QA using tiny PPMs and terminal receipt fixtures."""

import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("render_test_scene_audit", ROOT / "scripts/render_test_scene_audit.py")
QA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QA)


def case(name, feature="bloom", state="on", scene="bloom"):
    return {"name": name, "frames": 3, "fps": 60, "count": 1,
            "settings": {"render_test_scene": scene, f"features.{feature}": state == "on"},
            "comparison": {"kind": "feature_toggle", "feature": feature, "state": state}}


class RenderSceneAuditTest(unittest.TestCase):
    @staticmethod
    def authored_metadata(scene="asset-contact-shadow"):
        return {"render_test_scene": scene, "render_test_source_kind": "authored-assets",
                "render_test_asset_sources": ["heaven/nodes/house2.node", "heaven/nodes/nested.node"],
                "render_test_inputs": {"objects": 1, "meshes": 1, "material_records": 1, "grounds": 0},
                "scene_objects": [{"mesh": "heaven/meshes/house2.mesh"}],
                "render_test_material_bindings": [{"mesh": "heaven/meshes/house2.mesh", "surface": "roof",
                                                   "material": "building_tessellation_roof"}],
                "renderer_features": {"original_textures": 3, "hires_textures": 0, "texture_fallbacks": 0}}

    def test_authored_inputs_have_separate_coverage_and_no_synthetic_object_requirement(self):
        enabled = case("authored", feature="contact_shadows", scene="asset-contact-shadow")
        enabled["asset_source_nodes"] = ["heaven/nodes/house2.node"]
        metadata = self.authored_metadata()
        review = QA.activation(enabled, metadata, enabled["settings"], "")
        self.assertEqual(review["flags"], [])
        self.assertEqual(review["source_kind"], "authored-assets")
        self.assertTrue(review["asset_data_used"])
        self.assertIn("visibility still requires", review["scope"])
        self.assertNotIn("required_object_curved_receiver_missing", review["flags"])
        for mutation, flag in (
            ({"render_test_inputs": {"objects": True, "meshes": 1, "material_records": 1}}, "missing_or_empty_objects"),
            ({"render_test_material_bindings": []}, "authored_material_bindings_missing_or_empty"),
            ({"render_test_material_bindings": [{"mesh": "house2.mesh", "surface": "roof", "material": ""}]}, "authored_material_bindings_missing_or_empty"),
            ({"scene_objects": []}, "authored_scene_objects_missing"),
            ({"scene_objects": [{"mesh": "stone.mesh"}]}, "authored_scene_material_bindings_disagree"),
            ({"renderer_features": {"original_textures": 0, "hires_textures": 0, "texture_fallbacks": 0}}, "authored_textures_missing_or_empty"),
            ({"renderer_features": {"original_textures": 3, "hires_textures": 0, "texture_fallbacks": 1}}, "texture_fallbacks_reported"),
            ({"renderer_features": {"original_textures": 3, "hires_textures": 0}}, "authored_texture_fallback_count_missing_or_invalid"),
        ):
            with self.subTest(flag=flag):
                self.assertIn(flag, QA.activation(enabled, {**metadata, **mutation}, enabled["settings"], "")["flags"])
        failed = QA.activation(enabled, metadata, enabled["settings"], "Failed to load texture heaven/textures/roof.dds")
        self.assertIn("resource_load_errors_reported", failed["flags"])

    def test_authored_receipt_replays_actual_inputs_and_checks_every_temporal_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = root / "sources"
            for name in QA.AUDIT.PROCEDURAL_SOURCE_FILES + QA.AUDIT.AUTHORED_SOURCE_FILES:
                path = sources / name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture generator bytes")
            data = root / "assets"; data.mkdir()
            node = data / "root.node"; node.write_bytes(b"actual input bytes")
            enabled = case("authored", feature="contact_shadows", scene="asset-contact-shadow")
            enabled.update(count=2, asset_source_nodes=["heaven/nodes/house2.node"])
            config = {"width": 2, "height": 2, "time_of_day": 12, **enabled["settings"]}
            for index in range(2):
                image = QA.sequence_path(root, enabled["name"], index)
                image.write_bytes(b"P6\n2 2\n255\n" + bytes([80] * 12))
                metadata = {**self.authored_metadata(), "render_api": "Vulkan", "benchmark_score": False,
                            "presented_frames": 3 + index, "width": 2, "height": 2, "fixed_step": 1 / 60,
                            "simulation_time": (3 + index) / 60, "camera_fov": 60, "time_of_day": 12,
                            "tour_time": None, "tour_motion": False, "sequence_index": index, "sequence_count": 2,
                            "floating_islands_frozen": True}
                Path(str(image) + ".json").write_text(json.dumps(metadata))
                Path(str(image) + ".config.json").write_text(json.dumps(config))
            image = QA.sequence_path(root, enabled["name"], 0)
            log = root / "authored.log"; log.write_text("Capture complete\n")
            with patch.object(QA.AUDIT, "ROOT", sources):
                source = QA.AUDIT.source_fingerprint(data, [enabled])
                receipt = {"case": enabled, "input_config": config, "exit_code": 0, "success": True,
                           "api": "vulkan", "binary_sha256": "frozen", "log_sha256": QA.digest(log),
                           "source_data": source, **QA.AUDIT.verify_sequence(image, config, "vulkan", 3, enabled)}
                reviewed = QA.check_receipt(root, enabled, receipt, "frozen", "vulkan")
                self.assertTrue(reviewed["receipt_replay_passed"], reviewed["errors"])
                self.assertEqual(reviewed["source_provenance"]["source_kind"], "authored-assets")
                self.assertEqual(len(reviewed["authored_frame_input_reviews"]), 2)
                source_free = {**receipt, "source_data": {"kind": "procedural-render-test-scenes", "asset_data_used": False}}
                self.assertFalse(QA.check_receipt(root, enabled, source_free, "frozen", "vulkan")["receipt_replay_passed"])
                cache = {}
                with patch.object(QA.AUDIT, "data_fingerprint", wraps=QA.AUDIT.data_fingerprint) as hashed:
                    for _ in range(2):
                        self.assertTrue(QA.check_receipt(root, enabled, receipt, "frozen", "vulkan", cache)["receipt_replay_passed"])
                    self.assertEqual(hashed.call_count, 1)
                snapshot = root / "frozen-source"
                shutil.copytree(sources, snapshot)
                loader = sources / QA.AUDIT.AUTHORED_SOURCE_FILES[0]
                original_loader = loader.read_bytes()
                loader.write_bytes(b"new live loader")
                failed = QA.check_receipt(root, enabled, receipt, "frozen", "vulkan")
                self.assertIn("source changed", failed["errors"][0])
                restored = QA.check_receipt(root, enabled, receipt, "frozen", "vulkan", source_root=snapshot)
                self.assertTrue(restored["receipt_replay_passed"], restored["errors"])
                self.assertEqual(restored["source_provenance"]["source_root"], str(snapshot.resolve()))
                frozen_loader = snapshot / QA.AUDIT.AUTHORED_SOURCE_FILES[0]
                frozen_loader.write_bytes(b"tampered frozen loader")
                failed = QA.check_receipt(root, enabled, receipt, "frozen", "vulkan", source_root=snapshot)
                self.assertIn("source changed", failed["errors"][0])
                loader.write_bytes(original_loader)
                node.write_bytes(b"changed actual input")
                failed = QA.check_receipt(root, enabled, receipt, "frozen", "vulkan")
                self.assertIn("asset data changed", failed["errors"][0])
                node.write_bytes(b"actual input bytes")
                path = Path(str(QA.sequence_path(root, enabled["name"], 1)) + ".json")
                metadata = json.loads(path.read_text()); metadata["render_test_material_bindings"] = []
                path.write_text(json.dumps(metadata))
                refreshed = {**receipt, **QA.AUDIT.verify_sequence(image, config, "vulkan", 3, enabled)}
                failed = QA.check_receipt(root, enabled, refreshed, "frozen", "vulkan")
                self.assertIn("integrity failed at frame 1", failed["errors"][0])

    def test_source_root_option_preserves_one_or_two_capture_positionals(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            for directories in (["one"], ["one", "two"]):
                with self.subTest(directories=directories), patch.object(sys, "argv", [
                        "render_test_scene_audit", *directories, "--source-root", str(snapshot)]), \
                        patch.object(QA, "review_run", return_value=0) as review:
                    self.assertEqual(QA.main(), 0)
                    self.assertEqual(review.call_count, len(directories))
                    for call, directory in zip(review.call_args_list, directories):
                        self.assertEqual(call.args[0].captures, Path(directory))
                        self.assertEqual(call.args[0].source_root, snapshot.resolve())

    def test_authored_pairs_require_identical_selected_nodes(self):
        enabled = case("on", scene="asset-contact-shadow", feature="contact_shadows")
        enabled["asset_source_nodes"] = ["heaven/nodes/house2.node"]
        disabled = {**enabled, "name": "off", "settings": {**enabled["settings"], "features.contact_shadows": False},
                    "comparison": {**enabled["comparison"], "state": "off"}}
        self.assertEqual(len(QA.group_pairs([enabled, disabled])), 1)
        for mutation in ({"args": ["--camera-fov", "30"]},
                         {"settings": {**disabled["settings"], "texture_quality": "4k"}}):
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "Unmatched authored pair"):
                QA.group_pairs([enabled, {**disabled, **mutation}])
        disabled["asset_source_nodes"] = ["heaven/nodes/stone1.node"]
        with self.assertRaisesRegex(ValueError, "Unmatched asset_source_nodes"):
            QA.group_pairs([enabled, disabled])

    def test_authored_manifest_retains_nine_strict_pairs_and_temporal_counts(self):
        cases = json.loads((ROOT / "tests/visual/real_asset_test_scenes.json").read_text())
        self.assertEqual(len(cases), 18)
        self.assertEqual(sum(c["count"] for c in cases), 96)
        # The original four-group matrix stays fixed; the later airship
        # diagnostic has its own maintained manifest and coverage check below.
        self.assertEqual({c["settings"]["render_test_scene"] for c in cases}, {
            "asset-contact-shadow", "asset-foliage", "asset-windows-lamps", "asset-animation",
        })
        for c in cases:
            QA.AUDIT.validate_case(c)
            self.assertEqual(c["expected_metadata"]["render_test_source_kind"], "authored-assets")
            self.assertTrue(c["asset_source_nodes"])
        groups = QA.group_pairs(cases)
        self.assertEqual(len(groups), 9)
        self.assertTrue(all(set(group) == {"on", "off"} for group in groups.values()))
        # The new authored layer supplements the unchanged procedural catalog.
        procedural = json.loads((ROOT / "tests/visual/render_test_scenes.json").read_text())
        self.assertEqual(len(procedural), 87)
        self.assertEqual({c["settings"]["render_test_scene"] for c in procedural}, QA.AUDIT.PROCEDURAL_SCENES)

    def test_separate_airship_manifest_retains_strict_ao_pair_and_authored_catalog_coverage(self):
        cases = json.loads((ROOT / "tests/visual/airship_specular_ao.json").read_text())
        self.assertEqual(len(cases), 2)
        self.assertEqual(sum(c["count"] for c in cases), 4)
        self.assertEqual({c["settings"]["render_test_scene"] for c in cases}, {"asset-airship"})
        for c in cases:
            QA.AUDIT.validate_case(c)
            self.assertEqual(c["expected_metadata"]["render_test_source_kind"], "authored-assets")
            self.assertEqual(c["asset_source_nodes"], ["heaven/nodes/airship.node"])
        groups = QA.group_pairs(cases)
        self.assertEqual(set(groups), {("feature_toggle", "ambient_occlusion", "asset-airship-specular-ao")})
        self.assertTrue(all(set(group) == {"on", "off"} for group in groups.values()))
        baseline = json.loads((ROOT / "tests/visual/real_asset_test_scenes.json").read_text())
        self.assertEqual({c["settings"]["render_test_scene"] for c in baseline + cases},
                         QA.AUDIT.AUTHORED_SCENES)

    def test_pair_grouping_preserves_feature_identity_and_rejects_mismatched_inputs(self):
        enabled, disabled = case("enabled"), case("disabled", state="off")
        groups = QA.group_pairs([disabled, enabled, case("other", feature="grass", scene="foliage")])
        self.assertEqual(groups[("feature_toggle", "bloom")], {"on": enabled, "off": disabled})
        self.assertEqual(set(groups[("feature_toggle", "grass")]), {"on"})
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            QA.group_pairs([enabled, case("duplicate")])
        for field, value in (("frames", 4), ("count", 2), ("fps", 30), ("pose", [0, 0, 0, 0, 0])):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "Unmatched"):
                QA.group_pairs([enabled, {**disabled, field: value}])

    def test_identical_effect_is_flagged_but_identical_instancing_is_expected(self):
        pixels = 2, 2, bytes(12)
        metrics = QA.difference(pixels, pixels)
        self.assertEqual(QA.difference_status("bloom", metrics), "unexpected_identical_effect_review")
        self.assertEqual(QA.difference_status("gpu_instancing", metrics), "expected_exact_parity")
        self.assertEqual(QA.difference_status("gpu_skinning", metrics), "expected_exact_parity")

    def test_variants_keep_displaced_contact_hours_in_separate_pairs(self):
        cases = []
        for hour in (8, 16):
            for state in ("on", "off"):
                item = case(f"displaced-{hour}-{state}", feature="contact_shadows", state=state, scene="displacement")
                item["comparison"]["variant"] = f"displaced-{hour:02d}"
                item["settings"]["time_of_day"] = hour
                cases.append(item)
        groups = QA.group_pairs(cases + [case("ordinary-on", feature="contact_shadows", scene="contact-shadow"),
                                        case("ordinary-off", feature="contact_shadows", state="off", scene="contact-shadow")])
        self.assertEqual(set(groups), {("feature_toggle", "contact_shadows"),
                                      ("feature_toggle", "contact_shadows", "displaced-08"),
                                      ("feature_toggle", "contact_shadows", "displaced-16")})
        changed = {**cases[1], "settings": {**cases[1]["settings"], "time_of_day": 16}}
        with self.assertRaisesRegex(ValueError, "Unmatched time_of_day"):
            QA.group_pairs([cases[0], changed])
        for invalid in (True, "", "../other", "Displaced_08"):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "Invalid comparison variant"):
                QA.group_pairs([{**cases[0], "comparison": {**cases[0]["comparison"], "variant": invalid}}])

    def test_displaced_contacts_require_both_height_receivers_and_independent_controls(self):
        enabled = case("displaced", feature="contact_shadows", scene="displacement")
        metadata = {"render_test_inputs": {"objects": 5, "meshes": 5, "material_records": 5},
                    "scene_objects": [{"mesh": "coarse_height"}, {"mesh": "dense_ground_relief"},
                                      {"mesh": "height_blocker"}, {"mesh": "pom_height"}],
                    "renderer_features": {"texture_fallbacks": 0}}
        self.assertEqual(QA.activation(enabled, metadata, enabled["settings"], "")["flags"], [])
        for absent in ("coarse_height", "dense_ground_relief", "height_blocker", "pom_height"):
            incomplete = {**metadata, "scene_objects": [o for o in metadata["scene_objects"] if o["mesh"] != absent]}
            with self.subTest(absent=absent):
                self.assertIn(f"required_object_{absent}_missing",
                              QA.activation(enabled, incomplete, enabled["settings"], "")["flags"])

    def test_rgb8_metrics_do_not_wrap_and_nonzero_parity_requests_review(self):
        a, b = (2, 2, bytes(12)), (2, 2, bytes([255, 0, 0] + [0] * 9))
        metrics = QA.difference(a, b)
        self.assertEqual(metrics["max_channel_rgb8"], 255)
        self.assertEqual(metrics["mean_channel_rgb8"], 255 / 12)
        self.assertEqual(metrics["pixels_gt_8"], 1)
        self.assertEqual(metrics["bbox_gt_2"], [0, 0, 1, 1])
        self.assertEqual(QA.difference_status("gpu_skinning", metrics), "parity_difference_review")
        one_level = QA.difference(a, (2, 2, bytes([1] * 12)))
        self.assertEqual(QA.difference_status("clouds", one_level), "only_rgb8_level_variation_review")

    def test_provisioned_inputs_do_not_claim_visible_activation(self):
        enabled = case("grass", feature="grass", scene="foliage")
        metadata = {"render_test_inputs": {"objects": 2, "meshes": 2, "material_records": 2, "grounds": 1},
                    "renderer_features": {"texture_fallbacks": 0}}
        review = QA.activation(enabled, metadata, {"features.grass": True}, "Grass: deferred to first frame")
        self.assertIn("positive_construction_log_not_found", review["flags"])
        built = QA.activation(enabled, metadata, {"features.grass": True}, "Grass: 1800 blades across 1 patches")
        self.assertEqual(built["flags"], [])
        self.assertIn("visibility still requires", built["scope"])

    def test_missing_real_dynamic_scale_is_not_accepted_from_enabled_flag(self):
        enabled = case("scale", feature="dynamic_resolution", scene="output")
        metadata = {"render_test_inputs": {"objects": 1, "meshes": 1, "material_records": 1},
                    "renderer_features": {"dynamic_resolution": True}, "fixture_dynamic_resolution_fixed": True}
        self.assertIn("lower_render_scale_not_observed", QA.activation(enabled, metadata, {}, "")["flags"])
        metadata["render_resolution_scale"] = [.5, .5]
        self.assertEqual(QA.activation(enabled, metadata, {}, "")["flags"], [])

    def test_requested_msaa_without_active_msaa_is_flagged(self):
        enabled = case("msaa", feature="msaa", scene="anti-aliasing")
        metadata = {"render_test_inputs": {"objects": 1, "meshes": 1, "material_records": 1},
                    "renderer_features": {"msaa_requested": True, "msaa_active": False}}
        self.assertIn("active_msaa_state_disagrees", QA.activation(enabled, metadata, {"features.msaa": True}, "")["flags"])

    def test_one_group_cannot_silently_mix_renderer_or_api_identities(self):
        one = {"binary_sha256": "frozen", "api": "vulkan"}
        self.assertEqual(QA.group_identity_errors([one, one]), [])
        self.assertTrue(QA.group_identity_errors([one, {**one, "binary_sha256": "other"}]))
        self.assertTrue(QA.group_identity_errors([one, {**one, "api": "opengl"}]))
        self.assertTrue(QA.group_identity_errors([{**one, "binary_sha256": None}]))

    def test_terminal_receipt_replay_detects_changed_pixels(self):
        with tempfile.TemporaryDirectory() as temporary:
            captures = Path(temporary)
            enabled = case("small")
            image = captures / "small.ppm"
            config = {"width": 2, "height": 2, "time_of_day": 12, **enabled["settings"]}
            image.write_bytes(b"P6\n2 2\n255\n" + bytes([80] * 12))
            metadata = {"render_api": "Vulkan", "benchmark_score": False, "presented_frames": 3,
                        "width": 2, "height": 2, "simulation_time": .05, "fixed_step": 1 / 60,
                        "camera_fov": 60, "time_of_day": 12, "tour_time": None, "tour_motion": False,
                        "render_test_inputs": {"objects": 1, "meshes": 1, "material_records": 1}}
            Path(str(image) + ".json").write_text(json.dumps(metadata))
            Path(str(image) + ".config.json").write_text(json.dumps(config))
            log = captures / "small.log"
            log.write_text("Capture complete\n")
            receipt = {"case": enabled, "input_config": config, "exit_code": 0, "success": True,
                       "api": "vulkan", "binary_sha256": "frozen", "log_sha256": QA.digest(log),
                       "source_data": {"kind": "procedural-render-test-scenes", "asset_data_used": False},
                       **QA.AUDIT.verify_sequence(image, config, "vulkan", 3, enabled)}
            self.assertTrue(QA.check_receipt(captures, enabled, receipt, "frozen", "vulkan")["receipt_replay_passed"])
            image.write_bytes(b"P6\n2 2\n255\n" + bytes([81] * 12))
            changed = QA.check_receipt(captures, enabled, receipt, "frozen", "vulkan")
            self.assertFalse(changed["receipt_replay_passed"])
            self.assertIn("image_sha256", changed["errors"][0])

    def test_shared_ppm_reader_rejects_truncation_and_non_rgb8(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "tiny.ppm"
            path.write_bytes(b"P6\n2 1\n255\n" + bytes([0, 1, 2, 3, 4, 5]))
            self.assertEqual(QA.AUDIT.read_ppm_rgb8(path), (2, 1, bytes([0, 1, 2, 3, 4, 5])))
            for invalid in (b"P6\n2 1\n255\n" + bytes(5), b"P6\n2 1\n65535\n" + bytes(6), b"P3\n2 1\n255\n" + bytes(6)):
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    path.write_bytes(invalid)
                    QA.AUDIT.read_ppm_rgb8(path)


if __name__ == "__main__":
    unittest.main()
