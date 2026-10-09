#!/usr/bin/env python3
"""Read-only receipt, input and image QA for procedural and authored render scenes.

The standard library verifies receipts and computes RGB8 metrics. Pillow is optional
for contact sheets. No renderer is launched.
Image warnings request review; they do not establish quality acceptance.
"""

import argparse
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

from types import SimpleNamespace

Image = ImageDraw = None


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("visual_audit", ROOT / "scripts/visual_audit.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)
PARITY_FEATURES = {"gpu_instancing", "gpu_skinning"}
COUNT_REQUIREMENTS = {
    "grass": "grounds", "massive_foliage": "grounds", "particles": "emitters",
    "heat_haze": "emitters", "lens_flares": "lights", "volumetric_fog": "lights",
    "point_light_shadows": "lights", "gpu_skinning": "animated_objects",
    "wind_debug": "cloth_bodies",
}
OBJECT_REQUIREMENTS = {
    "contact_shadows": ["curved_receiver", "thin_blocker"],
    "transparent_shadows": ["leaf_shadow_card"],
    "ground_relief": ["dense_ground_relief"],
    "clear_coat": ["lacquer"], "anisotropic": ["brushed"],
    "subsurface": ["leaf"], "cloth": ["cloth"],
    "colored_penumbra": ["pure_white_guard"],
    "depth_of_field": ["focus_0", "focus_1", "focus_2"],
}
SCENE_OBJECT_REQUIREMENTS = {
    ("contact_shadows", "displacement"): [
        "coarse_height", "dense_ground_relief", "height_blocker", "pom_height"],
}
CONSTRUCTION_PATTERNS = {
    "grass": r"Grass: (\d+) blades across (\d+) patches",
    "massive_foliage": r"MassiveFoliage: (\d+) explicit procedural grass roots",
    "particles": r"Particles: built (\d+) emitters \(smoke=(\d+), flame=(\d+), glow=(\d+)\) \+(\d+) heat-haze",
    "heat_haze": r"Particles: built (\d+) emitters \(smoke=(\d+), flame=(\d+), glow=(\d+)\) \+(\d+) heat-haze",
    "lens_flares": r"Lens flares: built (\d+) \((\d+) authored \+ (\d+) emission\)",
    "rain": r"Rain: built (\d+) streak quads",
    "clouds": r"Clouds: enabled",
    "volumetric_clouds": r"Volumetric clouds: raymarched layer initialized \((\d+) steps\)(?! \(disabled\))",
    "god_rays": r"God rays: volumetric light scattering initialized(?! \(disabled\))",
    "volumetric_fog": r"Volumetric fog: per-light ray-march initialized \((\d+)-tap, (\d+) lights max\)(?! \(disabled\))",
    "ssgi": r"SSGI: screen-space global illumination initialized \((\d+) samples\)(?! \(disabled\))",
}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sequence_path(captures, name, index):
    return captures / (f"{name}.ppm" if index == 0 else f"{name}.{index:06d}.ppm")


def image_pixels(path):
    return AUDIT.read_ppm_rgb8(path)


def percentile(histogram, fraction):
    count = sum(histogram)
    index = (count - 1) * fraction
    lower = int(index)
    upper = lower if index == lower else lower + 1
    values = []
    total = 0
    for value, number in enumerate(histogram):
        if total <= lower < total + number:
            values.append(value)
        if total <= upper < total + number:
            values.append(value)
        total += number
    return values[0] + (values[-1] - values[0]) * (index - lower)


def difference(a, b):
    width, height, pixels = a
    if (width, height) != b[:2]:
        raise ValueError(f"Image shape mismatch: {a[:2]} versus {b[:2]}")
    d = bytes(abs(x - y) for x, y in zip(pixels, b[2]))
    histogram = [0] * 256
    bounds = [width, height, -1, -1]
    for index in range(0, len(d), 3):
        value = max(d[index], d[index + 1], d[index + 2])
        histogram[value] += 1
        if value > 2:
            pixel_index = index // 3
            x, y = pixel_index % width, pixel_index // width
            bounds = [min(bounds[0], x), min(bounds[1], y),
                      max(bounds[2], x + 1), max(bounds[3], y + 1)]
    return {
        "mean_channel_rgb8": sum(d) / len(d), "max_channel_rgb8": max(d),
        "p95_pixel_max_rgb8": percentile(histogram, .95),
        "p99_pixel_max_rgb8": percentile(histogram, .99),
        "changed_pixels": sum(histogram[1:]), "pixels_gt_1": sum(histogram[2:]),
        "pixels_gt_2": sum(histogram[3:]), "pixels_gt_8": sum(histogram[9:]),
        "fraction_gt_2": sum(histogram[3:]) / (width * height),
        "bbox_gt_2": bounds if bounds[2] >= 0 else None,
    }


def difference_status(feature, metrics):
    if feature in PARITY_FEATURES:
        if metrics["max_channel_rgb8"] == 0:
            return "expected_exact_parity"
        if metrics["max_channel_rgb8"] <= 1:
            return "parity_small_rgb8_variation_review"
        return "parity_difference_review"
    if metrics["max_channel_rgb8"] == 0:
        return "unexpected_identical_effect_review"
    if metrics["pixels_gt_2"] == 0:
        return "only_rgb8_level_variation_review"
    return "visible_difference_requires_inspection"


def activation(case, metadata, config, log):
    comparison = case.get("comparison", {})
    feature, state = comparison.get("feature"), comparison.get("state")
    requested = None
    if comparison.get("kind") == "feature_toggle":
        if feature in {"color_grading", "physical_camera", "dynamic_resolution"}:
            requested = metadata.get("renderer_features", {}).get(feature)
        else:
            key = "heat_haze_enabled" if feature == "heat_haze" else f"features.{feature}"
            requested = config.get(key)
    elif comparison.get("kind") == "core_control" and feature == "tessellation":
        requested = config.get("tessellation.enabled")
    inputs = metadata.get("render_test_inputs", {})
    if not isinstance(inputs, dict):
        inputs = {}
    scene = case.get("settings", {}).get("render_test_scene")
    source_kind = AUDIT.fixture_source_kind(scene)
    authored = source_kind == "authored-assets"
    flags = []
    for field in ("objects", "meshes", "material_records"):
        if type(inputs.get(field)) is not int or inputs[field] <= 0:
            flags.append(f"missing_or_empty_{field}")
    if feature in COUNT_REQUIREMENTS:
        count = COUNT_REQUIREMENTS[feature]
        # Authored terrain is ordinary loaded geometry, not a synthetic ground
        # registration. Its actual resource/construction logs are still checked.
        if not (authored and count == "grounds") and (type(inputs.get(count)) is not int or inputs[count] <= 0):
            flags.append(f"required_input_{count}_empty")
    scene_objects = metadata.get("scene_objects", [])
    objects = {Path(obj["mesh"]).name for obj in scene_objects if isinstance(obj, dict) and isinstance(obj.get("mesh"), str)} if isinstance(scene_objects, list) else set()
    required_objects = ([] if authored else
                        SCENE_OBJECT_REQUIREMENTS.get((feature, scene), OBJECT_REQUIREMENTS.get(feature, [])))
    for label in required_objects:
        if label not in objects:
            flags.append(f"required_object_{label}_missing")
    if (comparison.get("kind") == "feature_toggle" or
            comparison.get("kind") == "core_control" and feature == "tessellation") and requested is not (state == "on"):
        flags.append("effective_switch_disagrees_with_pair")
    runtime = metadata.get("renderer_features", {})
    if not isinstance(runtime, dict):
        runtime = {}
    observed = {}
    bindings = metadata.get("render_test_material_bindings")
    if authored:
        try:
            AUDIT.verify_fixture_metadata(metadata, case)
        except ValueError as error:
            flags.append(f"authored_source_metadata_invalid: {error}")
        if not objects or "" in objects:
            flags.append("authored_scene_objects_missing")
        if (not isinstance(bindings, list) or not bindings or
                any(not isinstance(binding, dict) or
                    any(not isinstance(binding.get(key), str) or not binding[key].strip()
                        for key in ("mesh", "surface", "material")) for binding in bindings)):
            flags.append("authored_material_bindings_missing_or_empty")
        elif not objects.intersection({Path(binding["mesh"]).name for binding in bindings}):
            flags.append("authored_scene_material_bindings_disagree")
        texture_counts = [runtime.get(key) for key in ("original_textures", "hires_textures")]
        if (any(type(value) is not int or value < 0 for value in texture_counts) or
                sum(value for value in texture_counts if type(value) is int) <= 0):
            flags.append("authored_textures_missing_or_empty")
        if type(runtime.get("texture_fallbacks")) is not int or runtime["texture_fallbacks"] < 0:
            flags.append("authored_texture_fallback_count_missing_or_invalid")
    if feature == "msaa":
        observed = {key: runtime.get(key) for key in
                    ("msaa_requested", "msaa_active", "msaa_samples", "custom_msaa_resolve", "alpha_to_coverage")}
        if "msaa_active" not in runtime:
            observed["scope"] = "Legacy metadata exposes a resolved capability count, not active MSAA."
        elif runtime.get("msaa_requested") is not (state == "on") or runtime["msaa_active"] is not (state == "on"):
            flags.append("active_msaa_state_disagrees")
    elif feature == "refraction":
        observed = {"refraction_active": runtime.get("refraction_active")}
        if state == "on" and runtime.get("refraction_active") is not True:
            flags.append("refraction_not_active")
    elif feature == "dynamic_resolution":
        observed = {"fixed_fixture_scale": metadata.get("fixture_dynamic_resolution_fixed"),
                    "actual_render_scale": metadata.get("render_resolution_scale")}
        scale = observed["actual_render_scale"]
        if state == "on" and (observed["fixed_fixture_scale"] is not True or
                not isinstance(scale, list) or len(scale) != 2 or
                not all(type(v) in (float, int) and 0 < v < 1 for v in scale)):
            flags.append("lower_render_scale_not_observed")
    constructed = []
    pattern = CONSTRUCTION_PATTERNS.get(feature)
    if authored and feature == "massive_foliage":
        pattern = r"MassiveFoliage: (\d+) grass blades built \(target (\d+)\)"
    if pattern:
        constructed = [{"line": match.group(0), "counts": [int(v) for v in match.groups()]}
                       for match in re.finditer(pattern, log)]
        if state == "on" and not constructed:
            flags.append("positive_construction_log_not_found")
        if state == "on" and constructed and feature in {"grass", "massive_foliage", "particles", "lens_flares", "rain"}:
            if constructed[-1]["counts"][0] <= 0:
                flags.append("constructed_resource_count_zero")
        if state == "on" and feature == "heat_haze" and constructed and constructed[-1]["counts"][-1] <= 0:
            flags.append("heat_haze_quad_count_zero")
    if runtime.get("texture_fallbacks", 0):
        flags.append("texture_fallbacks_reported")
    resource_errors = [line for line in log.splitlines() if re.search(
        r"(?:material|texture).*?(?:failed to load|unavailable|not found|missing).*?(?:disabled|skipping|failed)|FEngine.*panic"
        r"|(?:failed to (?:load|open)|cannot (?:load|open)).*?(?:mesh|material|texture|\.node)"
        r"|(?:mesh|material|texture|\.node).*?(?:failed to load|not found|unavailable)", line, re.I)]
    if resource_errors:
        flags.append("resource_load_errors_reported")
    return {"feature": feature, "state": state, "effective_switch": requested,
            "source_kind": source_kind, "asset_data_used": authored,
            "asset_sources": metadata.get("render_test_asset_sources", []),
            "material_bindings": bindings if authored else None,
            "input_counts": inputs, "runtime_observer": observed,
            "construction_log_evidence": constructed, "resource_error_lines": resource_errors,
            "flags": flags,
            "scope": "Provisioned inputs and observed resource setup; visibility still requires the registered image comparison."}


def check_receipt(captures, case, receipt, binary_hash, expected_api, source_cache=None, source_root=None):
    result = {"name": case["name"], "scene": case["settings"]["render_test_scene"],
              "exit_code": receipt.get("exit_code"), "runner_verified": receipt.get("success"),
              "wall_seconds": receipt.get("wall_seconds"), "errors": []}
    try:
        if receipt.get("exit_code") != 0 or receipt.get("success") is not True:
            raise ValueError(f"Non-success receipt: {receipt.get('error', '')}")
        if receipt.get("case") != case:
            raise ValueError("Receipt case differs from current selected manifest")
        if expected_api and receipt.get("api") != expected_api:
            raise ValueError("Receipt API differs from selected API")
        if binary_hash and receipt.get("binary_sha256") != binary_hash:
            raise ValueError("Receipt renderer hash differs from frozen binary")
        image = sequence_path(captures, case["name"], 0)
        products = AUDIT.verify_sequence(image, receipt["input_config"], receipt["api"], case["frames"], case)
        for key, value in products.items():
            if receipt.get(key) != value:
                raise ValueError(f"Replayed {key} differs from stored receipt")
        log_path = captures / f"{case['name']}.log"
        if digest(log_path) != receipt.get("log_sha256"):
            raise ValueError("Log changed after terminal receipt")
        result["source_provenance"] = AUDIT.verify_source_fingerprint(
            receipt.get("source_data"), case, source_cache, source_root)
        metadata = products["capture"]
        config = json.loads(Path(str(image) + ".config.json").read_text())
        result["activation"] = activation(case, metadata, config, log_path.read_text())
        if result["activation"]["source_kind"] == "authored-assets":
            frame_reviews = []
            for index in range(case.get("count", 1)):
                frame_metadata = (metadata if index == 0 else json.loads(
                    Path(str(sequence_path(captures, case["name"], index)) + ".json").read_text()))
                frame_review = activation(case, frame_metadata, config, log_path.read_text())
                frame_reviews.append({"index": index, "flags": frame_review["flags"]})
                if frame_review["flags"]:
                    raise ValueError(f"Authored fixture input/resource integrity failed at frame {index}: {frame_review['flags']}")
            result["authored_frame_input_reviews"] = frame_reviews
        result["capture"] = {key: metadata.get(key) for key in
                             ("render_api", "gpu", "driver", "driver_version", "render_test_scene",
                              "width", "height", "time_of_day", "simulation_time", "presented_frames",
                              "render_resolution_scale", "renderer_features", "render_test_source_kind",
                              "render_test_asset_sources", "render_test_inputs", "render_test_material_bindings")}
        result["frames_replayed"] = case.get("count", 1)
    except (OSError, ValueError, TypeError, KeyError) as error:
        result["errors"].append(str(error))
    result["receipt_replay_passed"] = not result["errors"]
    return result


def compare_pair(captures, feature, on, off, sequence=False):
    count = min(on.get("count", 1), off.get("count", 1)) if sequence else 1
    frame_metrics = []
    for index in range(count):
        a = image_pixels(sequence_path(captures, on["name"], index))
        b = image_pixels(sequence_path(captures, off["name"], index))
        frame_metrics.append({"index": index, **difference(a, b)})
    first = frame_metrics[0]
    aggregate = {"mean_of_frame_means_rgb8": sum(m["mean_channel_rgb8"] for m in frame_metrics) / count,
                 "maximum_over_frames_rgb8": max(m["max_channel_rgb8"] for m in frame_metrics),
                 "frames_with_pixels_gt_2": sum(m["pixels_gt_2"] > 0 for m in frame_metrics)}
    status = difference_status(feature, first)
    if count > 1 and status == "unexpected_identical_effect_review" and aggregate["maximum_over_frames_rgb8"] > 0:
        status = "first_frame_identical_later_difference_review"
    return {"feature": feature, "variant": on["comparison"].get("variant"),
            "scene": on["settings"]["render_test_scene"],
            "source_kind": AUDIT.fixture_source_kind(on["settings"]["render_test_scene"]),
            "on": on["name"], "off": off["name"], "compared_frames": count,
            "expectation": "geometry parity" if feature in PARITY_FEATURES else "visible effect change",
            "status": status, "first_frame": first, "aggregate": aggregate, "per_frame": frame_metrics}


def group_pairs(cases):
    groups = defaultdict(dict)
    for case in cases:
        comparison = case.get("comparison", {})
        state = comparison.get("state")
        if state not in {"on", "off"}:
            continue
        key = comparison["kind"], comparison["feature"]
        variant = comparison.get("variant")
        if variant is not None:
            if not isinstance(variant, str) or not variant or not re.fullmatch(r"[a-z0-9-]+", variant):
                raise ValueError("Invalid comparison variant")
            key += (variant,)
        if state in groups[key]:
            raise ValueError(f"Duplicate {state} comparison for {key}")
        groups[key][state] = case
    for key, group in groups.items():
        if set(group) == {"on", "off"}:
            on, off = group["on"], group["off"]
            for field in ("pose", "frames", "fps", "count", "asset_source_nodes"):
                if on.get(field) != off.get(field):
                    raise ValueError(f"Unmatched {field} in {key}")
            if on["settings"]["render_test_scene"] != off["settings"]["render_test_scene"]:
                raise ValueError(f"Unmatched scene in {key}")
            if on["settings"].get("time_of_day") != off["settings"].get("time_of_day"):
                raise ValueError(f"Unmatched time_of_day in {key}")
            if AUDIT.fixture_source_kind(on["settings"]["render_test_scene"]) == "authored-assets":
                if on.get("args", []) != off.get("args", []):
                    raise ValueError(f"Unmatched authored pair arguments in {key}")
                feature = on["comparison"]["feature"]
                switch = ("tessellation.enabled" if on["comparison"]["kind"] == "core_control" and feature == "tessellation"
                          else "heat_haze_enabled" if feature == "heat_haze" else f"features.{feature}")
                on_settings, off_settings = dict(on["settings"]), dict(off["settings"])
                if on_settings.pop(switch, None) is not True or off_settings.pop(switch, None) is not False:
                    raise ValueError(f"Authored pair does not isolate its declared switch in {key}")
                if on_settings != off_settings:
                    raise ValueError(f"Unmatched authored pair settings in {key}")
    return groups


def thumbnail(path):
    with Image.open(path) as image:
        return image.convert("RGB").resize((320, 180), Image.Resampling.LANCZOS)


def contact_sheet(captures, output, scene, cases, pairs):
    used = set()
    rows = []
    for pair in pairs:
        if pair["scene"] != scene:
            continue
        on_path, off_path = captures / f"{pair['on']}.ppm", captures / f"{pair['off']}.ppm"
        a, b = image_pixels(on_path), image_pixels(off_path)
        difference_image = Image.frombytes("RGB", a[:2], bytes(min(255, abs(x - y) * 4) for x, y in zip(a[2], b[2])))
        label = pair["feature"] + (f" ({pair['variant']})" if pair.get("variant") else "")
        rows.append([(thumbnail(on_path), f"{label} ON"),
                     (thumbnail(off_path), f"{label} OFF"),
                     (difference_image.resize((320, 180), Image.Resampling.LANCZOS),
                      f"diff x4; mean {pair['first_frame']['mean_channel_rgb8']:.4g}; max {pair['first_frame']['max_channel_rgb8']:.0f}")])
        used.update((pair["on"], pair["off"]))
    extras = [case for case in cases if case["name"] not in used and (captures / f"{case['name']}.ppm").is_file()]
    for begin in range(0, len(extras), 3):
        rows.append([(thumbnail(captures / f"{case['name']}.ppm"), case["name"])
                     for case in extras[begin:begin + 3]])
    if not rows:
        return None
    canvas = Image.new("RGB", (984, 36 + len(rows) * 215), (20, 23, 28))
    draw = ImageDraw.Draw(canvas)
    draw.text((10, 10), f"{scene}: registered first-frame inputs, differences amplified x4", fill="white")
    for row_index, row in enumerate(rows):
        for column, (image, label) in enumerate(row):
            x, y = 8 + column * 328, 36 + row_index * 215
            canvas.paste(image, (x, y))
            draw.text((x + 2, y + 184), label, fill="white")
    path = output / f"scene-{scene}.png"
    canvas.save(path)
    return str(path)


def review_run(args):
    captures = args.captures.resolve()
    output = (args.output or captures / "qa").resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(args.manifest.read_text())
    receipts_path = captures / "results.json"
    receipts = json.loads(receipts_path.read_text())
    by_name = {r["case"]["name"]: r for r in receipts}
    missing = [c["name"] for c in manifest if c["name"] not in by_name]
    expected_names = {c["name"] for c in manifest}
    unexpected = sorted(set(by_name) - expected_names)
    duplicate = len(by_name) != len(receipts)
    cases = [c for c in manifest if c["name"] in by_name]
    binary_hash = digest(args.binary) if args.binary else None
    identity_errors = group_identity_errors(receipts)
    source_cache = {}
    source_root = getattr(args, "source_root", None)
    checked = [check_receipt(captures, c, by_name[c["name"]], binary_hash, args.api,
                             source_cache, source_root) for c in cases]
    valid = {c["name"] for c in checked if c["receipt_replay_passed"]}
    groups = group_pairs(cases)
    pairs = [compare_pair(captures, key[1], group["on"], group["off"], args.sequence)
             for key, group in groups.items()
             if set(group) == {"on", "off"} and all(c["name"] in valid for c in group.values())]
    sheets = {}
    for scene in sorted({c["settings"]["render_test_scene"] for c in cases}) if args.contact_sheets else []:
        subset = [c for c in cases if c["settings"]["render_test_scene"] == scene and c["name"] in valid]
        sheets[scene] = contact_sheet(captures, output, scene, subset, pairs)
    structural_pass = not identity_errors and not unexpected and not duplicate and len(valid) == len(cases) and (args.partial or not missing)
    if args.terminal_exit_code is not None and args.terminal_exit_code != 0:
        structural_pass = False
    flags = [{"case": c["name"], "flags": c.get("activation", {}).get("flags", []), "errors": c["errors"]}
             for c in checked if c["errors"] or c.get("activation", {}).get("flags")]
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(), "helper_sha256": digest(__file__),
        "scope": "Receipt integrity, actual inputs/resource setup and SDR RGB8 pair metrics; no automatic visual, performance or original-Heaven bug acceptance.",
        "manifest": str(args.manifest.resolve()), "manifest_sha256": digest(args.manifest),
        "results": str(receipts_path), "results_sha256": digest(receipts_path),
        "selected_api": args.api, "frozen_binary_sha256_checked": binary_hash,
        "fixture_source_root": str(AUDIT.fixture_source_root(source_root)),
        "fixture_source_scope": "declared-snapshot" if source_root is not None else "current",
        "fixture_source_verification": "Recorded catalog/loader hashes are checked against this source tree; actual asset/input paths and their hashes are unchanged.",
        "external_terminal_exit_code": args.terminal_exit_code,
        "terminal_status_scope": "The supplied external exit is separate from per-case exit codes; without it only case terminal receipts are verified.",
        "sequence_metrics": args.sequence, "contact_sheets_requested": args.contact_sheets,
        "partial": args.partial, "manifest_cases": len(manifest), "present_cases": len(cases),
        "verified_cases": len(valid), "verified_frames": sum(c.get("frames_replayed", 0) for c in checked),
        "missing_cases": missing, "unexpected_cases": unexpected, "duplicate_receipts": duplicate,
        "group_identity_errors": identity_errors,
        "structural_pass": structural_pass,
        "renderer_identities": sorted({r.get("binary_sha256", "") for r in receipts}),
        "driver_identities": sorted({(r.get("capture", {}).get("render_api", ""), r.get("capture", {}).get("gpu", ""),
                                      r.get("capture", {}).get("driver", ""), r.get("capture", {}).get("driver_version", "")) for r in receipts}),
        "case_reviews": checked, "activation_or_receipt_flags": flags,
        "coverage_by_source_kind": {
            kind: {"verified_cases": sum(c["receipt_replay_passed"] and c.get("source_provenance", {}).get("source_kind") == kind for c in checked),
                   "verified_frames": sum(c.get("frames_replayed", 0) for c in checked
                                          if c["receipt_replay_passed"] and c.get("source_provenance", {}).get("source_kind") == kind)}
            for kind in ("procedural", "authored-assets")},
        "pairs": pairs, "pair_review_flags": [p["feature"] for p in pairs if p["status"] != "visible_difference_requires_inspection" and p["status"] != "expected_exact_parity"],
        "contact_sheets": sheets,
        "limits": ["Nonzero differences can include temporal output dithering and are not automatically causal feature evidence.",
                   "Construction logs prove resource setup, not pixel visibility or a per-frame shader branch.",
                   "Instancing/skinning should preserve geometry and illumination; their parity is reviewed separately.",
                   "RGB8-only or identical effect pairs require inspection rather than a relaxed threshold or an inferred pass."]}
    report_path = output / "qa-summary.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    lines = [f"Replayed {len(valid)}/{len(cases)} present cases ({report['verified_frames']} frames); manifest {len(manifest)} cases.",
             f"Structural pass: {structural_pass}; external runner exit: {args.terminal_exit_code}.",
             f"Missing {len(missing)}, unexpected {len(unexpected)}, activation/receipt flagged {len(flags)}.", "", "Feature pair | mean RGB8 | max RGB8 | pixels >2 | interpretation"]
    for pair in pairs:
        m = pair["first_frame"]
        label = pair["feature"] + (f" ({pair['variant']})" if pair.get("variant") else "")
        lines.append(f"{label} | {m['mean_channel_rgb8']:.6g} | {m['max_channel_rgb8']:.6g} | {m['pixels_gt_2']} | {pair['status']}")
    (output / "qa-summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"Saved {report_path}")
    return 0 if structural_pass else 1


def group_identity_errors(receipts):
    """A capture group uses one renderer and API; two APIs belong in separate dirs."""
    errors = []
    for field in ("binary_sha256", "api"):
        values = {receipt.get(field) for receipt in receipts}
        if len(values) != 1 or None in values or "" in values:
            errors.append(f"Capture group does not retain one {field}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+", help="One or two visual_audit result directories")
    parser.add_argument("--manifest", type=Path, default=ROOT / "tests/visual/render_test_scenes.json")
    parser.add_argument("--output", type=Path, help="Report root; two inputs get separate named subdirectories")
    parser.add_argument("--api", choices=("vulkan", "opengl"), help="Optional expected API for a single input")
    parser.add_argument("--binary", type=Path, help="Optional frozen renderer identity to check")
    parser.add_argument("--source-root", type=Path,
                        help="Verify recorded fixture catalog/loader hashes against an immutable source snapshot instead of the live checkout")
    parser.add_argument("--terminal-exit-code", type=int, action="append", default=[], help="One external runner exit per input; case receipt exits are always checked")
    parser.add_argument("--partial", action="store_true", help="Allow absent manifest cases for a partial capture group")
    parser.add_argument("--sequence", action="store_true", help="Measure all corresponding sequence images; integrity is always checked for every image")
    parser.add_argument("--contact-sheets", action="store_true", help="Create optional PNG sheets; requires Pillow")
    args = parser.parse_args()
    if args.source_root is not None:
        try:
            args.source_root = AUDIT.fixture_source_root(args.source_root)
        except ValueError as error:
            parser.error(str(error))
    if len(args.captures) > 2:
        parser.error("Choose one or two result directories")
    if args.api and len(args.captures) != 1:
        parser.error("--api applies to a single result directory; each captured API is verified from its receipts")
    if args.terminal_exit_code and len(args.terminal_exit_code) != len(args.captures):
        parser.error("Supply one --terminal-exit-code for each result directory")
    if args.contact_sheets:
        global Image, ImageDraw
        try:
            from PIL import Image, ImageDraw
        except ImportError:
            parser.error("Contact sheets require Pillow; omit --contact-sheets for dependency-free JSON/text QA")
    status = 0
    for index, captures in enumerate(args.captures):
        output = args.output / captures.name if args.output and len(args.captures) > 1 else args.output
        run_args = SimpleNamespace(**{**vars(args), "captures": captures, "output": output,
                                      "terminal_exit_code": args.terminal_exit_code[index] if args.terminal_exit_code else None})
        status = max(status, review_run(run_args))
    return status


if __name__ == "__main__":
    raise SystemExit(main())
