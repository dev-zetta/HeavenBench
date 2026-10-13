#!/usr/bin/env python3
"""Capture real HeavenBench scenes sequentially; retain commands and exit codes.

Uses only the standard library. This is a diagnostic workload, not a benchmark.
The application writes lossless PPM plus camera/driver and effective-config JSON.
These are real application images. Generated images, provider activation and
display cadence require separate native frame-generation diagnostics.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
PROCEDURAL_SCENES = frozenset({
    "contact-shadow", "shadow-cascade", "material-models", "transparency",
    "displacement", "anti-aliasing", "ambient-occlusion", "reflections",
    "lighting", "bloom", "depth-of-field", "atmosphere", "foliage", "particles",
    "rain", "animation", "output",
})
AUTHORED_SCENES = frozenset({
    "asset-contact-shadow", "asset-foliage", "asset-windows-lamps", "asset-animation",
    "asset-airship",
})
PROCEDURAL_SOURCE_FILES = (
    "benchmark/src/engine/render_test_scene_catalog.h",
    "benchmark/src/engine/render_test_scene_catalog.cpp",
    "benchmark/src/engine/render_test_scene.h",
    "benchmark/src/engine/render_test_scene.cpp",
    "benchmark/src/rhi/render_test_scene_assets.h",
    "benchmark/src/rhi/render_test_scene_assets.cpp",
)
AUTHORED_SOURCE_FILES = (
    "benchmark/src/engine/scene_rhi.h",
    "benchmark/src/engine/scene_rhi.cpp",
    "benchmark/src/engine/material_rhi.h",
    "benchmark/src/engine/material_rhi.cpp",
    "benchmark/src/engine/asset_file_system.h",
    "benchmark/src/engine/asset_file_system.cpp",
    "tests/visual/real_asset_test_scenes.json",
)
# Per-case arguments configure the renderer only. run.sh consumes these before
# forwarding arguments, so accepting --build could replace the binary after
# its receipt fingerprint was computed, and --x11 could override the API route.
WRAPPER_FLAGS = {"--build", "-b", "--build-dir", "--compiler", "--cpu-arch",
                 "--gpu", "--list-gpus", "--wayland", "--x11", "--help", "-h"}
SCENE_STARTS = [0, 17, 22.7, 28.4, 34.1, 45.45, 68.1, 73.8, 85.15,
                96.5, 107.85, 119.2, 129.95, 135.65, 147, 152.7, 158.4,
                169.75, 181.1, 192.45, 198.15, 209.5, 220.85, 232.2,
                243.55, 249.25]

# One setting changes per pair. Rain stays commanded for precipitation and wet
# material comparisons; sky/lighting remains identical when rendering is hidden.
# Close material/receiver views come from the supplemental GPU audit: the old
# generic street poses often contained no pixels using the feature under test.
FEATURE_VIEWS = {'grass': 68.9,
 'massive_foliage': 68.9,
 'ground_relief': {'pose': [-8, -17.5, 23.75, 90, -14],
                   'args': ['--camera-fov', '50'],
                   'settings': {'time_of_day': 8,
                                'features.grass': False,
                                'features.massive_foliage': False}},
 'particles': {'pose': [-62, 62, 20, -30, -20], 'frames': 240, 'settings': {'time_of_day': 12}},
 'heat_haze': {'pose': [-53.5, 59.5, 17.5, -45, 0],
               'args': ['--camera-fov', '40'],
               'frames': 240,
               'settings': {'time_of_day': 12, 'features.particles': True}},
 'god_rays': {'pose': [4.23908, -18.56104, 24.2079, 34.37747, 30],
              'settings': {'time_of_day': 8, 'features.volumetric_fog': False}},
 'volumetric_fog': 193.25,
 'clouds': {'pose': [0, -45, 60, 0, -35],
            'settings': {'features.volumetric_clouds': False, 'time_of_day': 12}},
 'sky': 1,
 'volumetric_clouds': {'pose': [0, -45, 60, 0, -35],
                       'settings': {'time_of_day': 12,
                                    'features.clouds': False,
                                    'features.volumetric_fog': False}},
 'lens_flares': {'pose': [-50.11721, 46.85126, 13.15872, 180, 0],
                 'args': ['--camera-fov', '40'],
                 'settings': {'time_of_day': 0,
                              'features.volumetric_fog': False,
                              'features.bloom': False,
                              'heat_haze_enabled': False}},
 'rain': 18,
 'wet_reflections': 193.25,
 'soft_shadows': 98,
 'contact_shadows': {'pose': [-133.3, -89.5, 22, -135.78752, -23.035],
                     'args': ['--camera-fov', '50'],
                     'frames': 180,
                     'settings': {'time_of_day': 9,
                                  'features.ambient_occlusion': False,
                                  'quality.contact_shadow_level': 2}},
 'point_light_shadows': {'pose': [10, 3, 29.5, 140.85601, -11.22174],
                         'args': ['--camera-fov', '60'],
                         'frames': 180,
                         'settings': {'time_of_day': 22,
                                      'quality.shadow_light_budget': 16,
                                      'features.bloom': False,
                                      'features.lens_flares': False}},
 'msaa': 18,
 'refraction': {'pose': [7.15, 5.9, 31.5, 136.58299, -7.05664],
                'args': ['--camera-fov', '35'],
                'frames': 180,
                'settings': {'time_of_day': 12, 'features.bloom': False, 'features.lens_flares': False}},
 'reflections': 193.25,
 'ssgi': {'pose': [-56, 44, 12.5, 59.03624, -16.25392],
          'args': ['--camera-fov', '60'],
          'frames': 180,
          'settings': {'time_of_day': 15, 'heat_haze_enabled': False, 'quality.ssgi_quality': 2}},
 'clear_coat': {'pose': [15, -0.5, 31.3, -149.68879, -10.42822],
                'args': ['--camera-fov', '50'],
                'frames': 180,
                'settings': {'time_of_day': 14}},
 'anisotropic': {'pose': [-133.3, -89.5, 22, -135.78752, -19.28369],
                 'args': ['--camera-fov', '45'],
                 'frames': 180,
                 'settings': {'time_of_day': 14}},
 'subsurface': 68.9,
 'cloth': {'pose': [-12.47, -4.23, 37, -163.03707, -0.01527],
           'args': ['--camera-fov', '45'],
           'frames': 180,
           'settings': {'time_of_day': 9}},
 'transparent_shadows': {'pose': [7.2, -59.5, 16.6, 38.65981, -27.50055],
                         'args': ['--camera-fov', '55'],
                         'frames': 180,
                         'settings': {'time_of_day': 10,
                                      'features.grass': False,
                                      'features.massive_foliage': False}},
 'colored_penumbra': 98,
 'specular_aa': 98,
 'gpu_instancing': 18,
 'gpu_skinning': 46.5,
 'bloom': {'pose': [-16.55995, 28.88356, 22.25873, -15.58437, 0],
           'args': ['--camera-fov', '30'],
           'settings': {'time_of_day': 21}},
 'ambient_occlusion': 98,
 'taa': 18,
 'depth_of_field': 170.55}


def cases(suite):
    result = []
    if suite in ("tour", "all"):
        for index, start in enumerate(SCENE_STARTS):
            result.append({"name": f"tour-{index + 1:02}", "tour_time": start + 0.8,
                           "settings": {"weather.rain_intensity": 0.0}})
    if suite in ("features", "all"):
        for feature, view in FEATURE_VIEWS.items():
            key = "heat_haze_enabled" if feature == "heat_haze" else f"features.{feature}"
            settings = {"weather.rain_intensity": 0.65 if feature in ("rain", "wet_reflections") else 0.0}
            camera = view if isinstance(view, dict) else {"tour_time": view}
            settings.update(camera.get("settings", {}))
            # Massive foliage replaces regular grass at startup. Isolate each
            # geometry path so the regular-grass pair actually exercises it.
            if feature == "grass":
                settings["features.massive_foliage"] = False
            elif feature == "massive_foliage":
                settings["features.grass"] = False
            for enabled in (True, False):
                result.append({"name": f"{feature}-{'on' if enabled else 'off'}",
                               **{key: value for key, value in camera.items() if key != "settings"},
                               "settings": {**settings, key: enabled},
                               **({"frames": 900} if feature == "wet_reflections" else {})})
        for enabled in (True, False):
            result.append({"name": f"tessellation-{'on' if enabled else 'off'}",
                           "pose": [-131, -89, 23, 210, -24],
                           "settings": {"tessellation.enabled": enabled,
                                        "weather.rain_intensity": 0.0, "time_of_day": 12}})
        for feature, view in (("color-grading", 18), ("physical-camera", 18),
                              ("dynamic-resolution", 18),
                              ("wind-debug", {"pose": [-12.47, -4.23, 37, -163.03707, -0.01527],
                                              "args": ["--camera-fov", "55"], "frames": 180,
                                              "settings": {"time_of_day": 9, "features.cloth": True}})):
            camera = view if isinstance(view, dict) else {"tour_time": view}
            controls = {
                "color-grading": ["--grading-saturation", "0.3", "--white-balance", "0.3,-0.15"],
                "physical-camera": ["--aperture", "4", "--shutter", "0.002", "--iso", "100"],
                "dynamic-resolution": ["--dynamic-resolution-quality", "0.5"],
                "wind-debug": [],
            }[feature]
            for enabled in (True, False):
                expected = {}
                if feature != "wind-debug":
                    expected[f"renderer_features.{feature.replace('-', '_')}"] = enabled
                if feature == "color-grading":
                    expected.update({"runtime_settings.grading_saturation": 0.3,
                                     "runtime_settings.grading_temperature": 0.3,
                                     "runtime_settings.grading_tint": -0.15})
                elif feature == "physical-camera":
                    expected.update({"runtime_settings.aperture": 4,
                                     "runtime_settings.shutter": 0.002,
                                     "runtime_settings.iso": 100})
                elif feature == "dynamic-resolution":
                    expected["runtime_settings.dynamic_resolution_min_scale"] = 0.5
                result.append({"name": f"{feature}-{'on' if enabled else 'off'}",
                               **{key: value for key, value in camera.items() if key not in ("args", "settings")},
                               "args": camera.get("args", []) + controls + ([f"--{feature}"] if enabled else []),
                               "settings": {"weather.rain_intensity": 0.0, **camera.get("settings", {})},
                               "expected_metadata": expected})
    if suite in ("variants", "all"):
        for mapper in ("aces", "agx", "pbr-neutral", "gt7", "generic"):
            result.append({"name": f"tone-{mapper}", "tour_time": 193.25,
                           "args": ["--tone-mapper", mapper], "settings": {"weather.rain_intensity": 0.0},
                           "expected_metadata": {"runtime_settings.tone_mapper": {
                               "aces": "ACES", "agx": "AgX", "pbr-neutral": "PBRNeutral",
                               "gt7": "GT7", "generic": "Generic"}[mapper]}})
        for workflow in ("metallic-roughness", "specular-glossiness"):
            result.append({"name": f"material-{workflow}", "tour_time": 98,
                           "args": ["--material-workflow", workflow], "settings": {"weather.rain_intensity": 0.0}})
        for tier in ("original", "2k", "4k"):
            result.append({"name": f"textures-{tier}", "tour_time": 98,
                           "settings": {"texture_quality": tier, "weather.rain_intensity": 0.0},
                           "review_note": "Confirm texture-set availability and actual tier use in the log; a requested tier alone is not proof."})
        for knob in ("ao", "bloom", "dof", "taa", "soft-shadow", "contact-shadow", "cloud", "ssgi", "specular-aa"):
            for level in (0, 2):
                result.append({"name": f"quality-{knob}-{level}", "tour_time": 170.55 if knob in ("bloom", "dof") else 98,
                               "args": [f"--{knob}-quality", str(level)], "settings": {"weather.rain_intensity": 0.0},
                               "expected_metadata": {"runtime_settings.specular_aa_quality": level} if knob == "specular-aa" else {}})
        for samples in (1, 2, 4):
            result.append({"name": f"msaa-samples-{samples}", "tour_time": 18,
                           "settings": {"quality.msaa_samples": samples, "weather.rain_intensity": 0.0}})
        # Keep the main reflection pair under ordinary defaults to expose
        # cross-feature state bugs; this separate pair isolates SSR ownership.
        for enabled in (True, False):
            result.append({"name": f"reflections-isolated-{'on' if enabled else 'off'}", "tour_time": 193.25,
                           "settings": {"features.reflections": enabled, "features.wet_reflections": False,
                                        "weather.rain_intensity": 0.0}})
    return result


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def data_fingerprint(path):
    """Hash input bytes once per invocation; generated tessellation cache is excluded."""
    if path.is_file():
        return {"path": str(path), "sha256": sha256(path)}
    if not path.is_dir():
        raise ValueError(f"Asset input does not exist: {path}")
    digest = hashlib.sha256()
    count = 0
    for item in sorted(path.rglob("*")):
        relative = item.relative_to(path)
        if relative.parts[0] == "cache":
            continue
        if item.is_symlink() and item.is_dir():
            raise ValueError(f"Nested asset directory symlink cannot be fingerprinted: {item}")
        if item.is_file():
            digest.update(json.dumps([relative.as_posix(), sha256(item)]).encode())
            count += 1
    return {"path": str(path), "sha256": digest.hexdigest(), "files": count}


def fixture_source_kind(scene):
    """Only maintained procedural IDs may bypass actual asset provenance."""
    if scene is None or scene == "":
        return "original-assets"
    if not isinstance(scene, str):
        raise ValueError(f"Invalid render-test scene ID: {scene!r}")
    if scene in PROCEDURAL_SCENES:
        return "procedural"
    if scene in AUTHORED_SCENES:
        return "authored-assets"
    raise ValueError(f"Unknown render-test scene ID: {scene!r}")


def fixture_source_root(source_root=None):
    """Select only the catalog/loader source tree; asset paths stay authoritative."""
    root = Path(source_root).resolve() if source_root is not None else ROOT
    if not root.is_dir():
        raise ValueError(f"Fixture source root is not a directory: {root}")
    return root


def source_file_fingerprints(sources, source_root=None):
    files = {}
    root = fixture_source_root(source_root)
    for relative in sources:
        source = root / relative
        if not source.is_file():
            raise ValueError(f"Render-test scene source is missing: {source}")
        files[relative] = {"sha256": sha256(source), "size": source.stat().st_size}
    return files


def source_fingerprint(path, selected, source_root=None):
    """Fingerprint fixture generators and every actual asset input used by a group."""
    path = Path(path)
    scenes = [case.get("settings", {}).get("render_test_scene") for case in selected]
    kinds = [fixture_source_kind(scene) for scene in scenes]
    if not kinds or all(kind == "original-assets" for kind in kinds):
        return data_fingerprint(path)
    procedural_only = all(kind == "procedural" for kind in kinds)
    sources = PROCEDURAL_SOURCE_FILES
    asset_data = None
    if not procedural_only:
        asset_data = data_fingerprint(path)
        if (path.is_file() and path.stat().st_size == 0) or asset_data.get("files") == 0:
            raise ValueError(f"Asset input is empty: {path}")
        sources += AUTHORED_SOURCE_FILES
    result = {
        "kind": ("procedural-render-test-scenes" if procedural_only else
                 "authored-asset-render-test-scenes" if all(kind == "authored-assets" for kind in kinds)
                 else "mixed-render-test-scenes"),
        "scene_ids": sorted({scene for scene in scenes if scene}),
        "asset_data_used": not procedural_only, "requested_asset_path": str(path),
        "current_source_files": source_file_fingerprints(sources, source_root),
        "source_scope": "Current source identities; the separately recorded binary hash identifies the actual compiled renderer.",
    }
    if source_root is not None:
        result["source_root"] = str(fixture_source_root(source_root))
        result["source_scope"] = ("Declared fixture source snapshot identities; the separately recorded binary hash "
                                  "identifies the actual renderer, not a proof that it was built from this snapshot.")
    if asset_data is not None:
        result.update(asset_data=asset_data, includes_original_scene="original-assets" in kinds)
        # Loose material inheritance and default textures also read ../core.
        # ZIP/pack inputs are covered by their complete file identity instead.
        core = path.parent / "core" if path.is_dir() else None
        result["sibling_core_data"] = data_fingerprint(core) if core is not None and core.exists() else None
    return result


def relative_node_paths(value):
    if not isinstance(value, list) or not value:
        raise ValueError("Authored fixture source nodes are missing or empty")
    for item in value:
        if (not isinstance(item, str) or not item or "\\" in item or ":" in item or
                item.startswith("/") or any(part in ("", ".", "..") for part in item.split("/")) or
                not item.endswith(".node")):
            raise ValueError(f"Invalid relative authored source node: {item!r}")
    if len(value) != len(set(value)):
        raise ValueError("Authored fixture source nodes contain duplicates")
    return value


def verify_fixture_source_files(source, required, root, label):
    files = source.get("current_source_files")
    if not isinstance(files, dict) or not set(required).issubset(files):
        raise ValueError(f"{label} catalog/loader source identities are incomplete")
    for relative, fingerprint in files.items():
        if (not isinstance(relative, str) or Path(relative).is_absolute() or "\\" in relative or
                any(part in ("", ".", "..") for part in relative.split("/"))):
            raise ValueError("Unsafe fixture source identity path")
        path = root / relative
        if not path.is_file():
            raise ValueError(f"{label} catalog/loader source is missing: {path}")
        if fingerprint != {"sha256": sha256(path), "size": path.stat().st_size}:
            raise ValueError(f"{label} catalog/loader source changed: {relative}")


def verify_source_fingerprint(source, case, cache=None, source_root=None):
    """Replay authored data/generator hashes once, even for long temporal groups."""
    scene = case.get("settings", {}).get("render_test_scene")
    kind = fixture_source_kind(scene)
    if not isinstance(source, dict):
        raise ValueError("Capture source identity must be an object")
    declared = source.get("kind")
    root = fixture_source_root(source_root)
    if declared == "procedural-render-test-scenes":
        if kind != "procedural" or source.get("asset_data_used") is not False:
            raise ValueError("Capture incorrectly claims asset-independent fixture provenance")
        scenes = source.get("scene_ids")
        if scenes is not None and (not isinstance(scenes, list) or scene not in scenes or
                any(fixture_source_kind(value) != "procedural" for value in scenes)):
            raise ValueError("Asset-independent source identity contains an invalid scene family")
        # Legacy asset-free receipts retain their original default behavior.
        # An explicit snapshot must contain every recorded generator byte.
        if source_root is not None:
            verify_fixture_source_files(source, PROCEDURAL_SOURCE_FILES, root, "Procedural fixture")
        result = {"source_kind": kind, "asset_data_used": False,
                  "scope": "Procedural generator receipt; no authored asset input is required."}
        if source_root is not None:
            result.update(source_root=str(root), source_scope="declared-snapshot",
                          source_fingerprint_replayed=True)
        return result
    if declared not in {"authored-asset-render-test-scenes", "mixed-render-test-scenes"}:
        raise ValueError("Capture did not retain the required fixture source identity")
    if source.get("asset_data_used") is not True:
        raise ValueError("Authored/mixed fixture incorrectly claims asset-free data")
    scenes = source.get("scene_ids")
    if (not isinstance(scenes, list) or not scenes or not all(isinstance(value, str) for value in scenes) or
            len(scenes) != len(set(scenes)) or
            scene not in scenes):
        raise ValueError("Fixture scene is missing from source identity")
    kinds = {fixture_source_kind(value) for value in scenes}
    if "original-assets" in kinds:
        raise ValueError("Original scene must be declared separately from fixture IDs")
    includes_original = source.get("includes_original_scene")
    if type(includes_original) is not bool:
        raise ValueError("Fixture identity does not retain original-scene inclusion")
    expected = ("authored-asset-render-test-scenes" if kinds == {"authored-assets"} and not includes_original
                else "mixed-render-test-scenes")
    if declared != expected or (kinds == {"procedural"} and not includes_original):
        raise ValueError("Fixture source kind disagrees with the selected scene family")
    key = json.dumps({"source": source, "verification_source_root": str(root)}, sort_keys=True, allow_nan=False)
    if cache is None or key not in cache:
        asset = source.get("asset_data")
        if not isinstance(asset, dict) or asset.get("path") != source.get("requested_asset_path"):
            raise ValueError("Authored fixture asset path is missing or inconsistent")
        path = Path(asset["path"])
        actual = data_fingerprint(path)
        if (path.is_file() and path.stat().st_size == 0) or actual.get("files") == 0:
            raise ValueError(f"Asset input is empty: {path}")
        if actual != asset:
            raise ValueError("Authored fixture asset data changed after capture")
        if "sibling_core_data" not in source:
            raise ValueError("Authored fixture sibling core provenance is missing")
        core = path.parent / "core" if path.is_dir() else None
        current_core = data_fingerprint(core) if core is not None and core.exists() else None
        if current_core != source["sibling_core_data"]:
            raise ValueError("Authored fixture sibling core data changed after capture")
        verify_fixture_source_files(source, PROCEDURAL_SOURCE_FILES + AUTHORED_SOURCE_FILES,
                                    root, "Authored fixture")
        if cache is not None:
            cache[key] = True
    return {"source_kind": kind, "asset_data_used": kind != "procedural",
            "group_asset_data_used": True, "source_fingerprint_replayed": True,
            "source_root": str(root),
            "source_scope": "declared-snapshot" if source_root is not None else "current",
            "scope": "Actual authored data and selected fixture source hashes; counts and pixels require separate review."}


def verify_fixture_metadata(metadata, case):
    scene = case.get("settings", {}).get("render_test_scene")
    kind = fixture_source_kind(scene)
    # Preserve older procedural captures, whose asset-free receipt is explicit
    # but predates this metadata field. Authored captures have no legacy bypass.
    declared = metadata.get("render_test_source_kind")
    if kind == "authored-assets":
        if metadata.get("render_test_scene") != scene or declared != kind:
            raise ValueError("Authored fixture scene/source-kind metadata disagrees")
        actual = relative_node_paths(metadata.get("render_test_asset_sources"))
        required = case.get("asset_source_nodes")
        if required is not None and not set(relative_node_paths(required)).issubset(actual):
            raise ValueError("Authored fixture did not load every selected source node")
    elif kind == "procedural":
        if declared is not None and declared != kind:
            raise ValueError("Procedural fixture source-kind metadata disagrees")
        if metadata.get("render_test_asset_sources"):
            raise ValueError("Procedural fixture unexpectedly declares authored source nodes")


def runtime_fingerprint():
    # Do not record arbitrary environment values (which can contain secrets).
    prefixes = ("VK_", "MESA_", "DRI_", "GALLIUM_", "LIBGL_", "__GL_", "__NV_", "RADV_", "ANV_", "SDL_", "LP_")
    names = {"DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "HEAVEN_RESOURCE_DIR", "LD_LIBRARY_PATH", "LD_PRELOAD", "AMD_VULKAN_ICD"}
    environment = {key: value for key, value in os.environ.items() if key in names or key.startswith(prefixes)}
    paths = {ROOT / "scripts/run.sh", ROOT / "scripts/build_paths.sh", Path(__file__).resolve()}
    for base in (Path("/usr/lib"), Path("/usr/lib64"), Path("/usr/lib/x86_64-linux-gnu"),
                 Path("/usr/lib/aarch64-linux-gnu")):
        for pattern in ("libvulkan*.so*", "libGL*.so*", "libEGL*.so*", "libgallium*.so*", "libnvidia*.so*", "dri/*_dri.so"):
            paths.update(file.resolve() for file in base.glob(pattern) if file.is_file())
    for base in (Path("/usr/share/vulkan/icd.d"), Path("/etc/vulkan/icd.d")):
        paths.update(base.glob("*.json"))
    files = {str(path): sha256(path) for path in sorted(paths) if path.is_file()}
    boot = Path("/proc/sys/kernel/random/boot_id")
    return {"environment": environment, "driver_files": files,
            "kernel": os.uname().release, "boot_id": boot.read_text().strip() if boot.exists() else ""}


def validate_case(case, default_frames=90):
    if not isinstance(case, dict) or not isinstance(case.get("name"), str) or not re.fullmatch(r"[A-Za-z0-9_-]+", case["name"]):
        raise ValueError("Each case needs a unique safe name (letters, numbers, underscores or hyphens)")
    if not isinstance(case.get("settings", {}), dict) or not isinstance(case.get("args", []), list):
        raise ValueError(f"Invalid settings/args for {case['name']}")
    if not isinstance(case.get("expected_metadata", {}), dict):
        raise ValueError(f"Invalid metadata expectations for {case['name']}")
    scene = case.get("settings", {}).get("render_test_scene")
    if scene is not None and (not isinstance(scene, str) or
                              (scene and not re.fullmatch(r"[a-z0-9-]+", scene))):
        raise ValueError(f"Invalid procedural scene ID for {case['name']}")
    fixture_source_kind(scene)
    if "asset_source_nodes" in case:
        if fixture_source_kind(scene) != "authored-assets":
            raise ValueError("Only authored fixtures may request asset source nodes")
        relative_node_paths(case["asset_source_nodes"])
    if "pose" in case and "tour_time" in case:
        raise ValueError(f"Conflicting camera controls for {case['name']}")
    for key in ("pose", "pose_to"):
        if key in case:
            pose = case[key]
            if not isinstance(pose, list) or len(pose) != 5 or any(type(value) not in (int, float) or not math.isfinite(value) for value in pose) or not -89 <= pose[4] <= 89:
                raise ValueError(f"Invalid finite camera pose/pitch for {case['name']}")
    if "tour_time" in case and (type(case["tour_time"]) not in (int, float) or not math.isfinite(case["tour_time"]) or case["tour_time"] < 0):
        raise ValueError(f"Invalid tour time for {case['name']}")
    frames = case.get("frames", default_frames)
    if type(frames) is not int or not 1 <= frames <= 36000:
        raise ValueError(f"Invalid frame count for {case['name']}")
    count = case.get("count", 1)
    if type(count) is not int or not 1 <= count <= 600:
        raise ValueError(f"Invalid sequence count for {case['name']}")
    fps = case.get("fps", 60)
    if type(fps) not in (int, float) or not math.isfinite(fps) or not 1 <= fps <= 240:
        raise ValueError(f"Invalid simulation FPS for {case['name']}")
    realtime = case.get("realtime", False)
    if type(realtime) is not bool or (realtime and "fps" in case):
        raise ValueError(f"Invalid real-time capture mode for {case['name']}")
    if "pose_to" in case and ("pose" not in case or "tour_time" in case or realtime or frames + count - 1 < 2):
        raise ValueError(f"Camera endpoint needs a pose, fixed-step capture and at least two total frames for {case['name']}")
    reserved = {"--capture", "--capture-frames", "--capture-count", "--capture-fps", "--capture-realtime", "--capture-camera-to", "--profile", "--profile-seconds", "--camera-pose", "--camera-tour-time", "--config", "--data",
                "--benchmark", "--pack-assets", "--build-tessellation-cache", "--benchmark-loading", "--write-config", "--help", "--hdr", "--test-scene", "--list-test-scenes"}
    if any(not isinstance(argument, str) or argument in reserved | WRAPPER_FLAGS for argument in case.get("args", [])):
        raise ValueError(f"Case {case['name']} overrides a controlled startup action")
    capture_fov(case)
    if "--capture-tour-motion" in case.get("args", []) and "tour_time" not in case:
        raise ValueError(f"Moving capture needs a tour start for {case['name']}")
    # Python's JSON parser accepts NaN by default; reject it before a case can
    # generate a malformed config or enter a render loop.
    json.dumps(case, allow_nan=False)


def capture_fov(case):
    """The last explicit FOV wins, exactly as in the renderer's CLI parser."""
    arguments = case.get("args", [])
    result = None
    for index, argument in enumerate(arguments):
        if argument != "--camera-fov":
            continue
        try:
            result = float(arguments[index + 1])
        except (IndexError, ValueError) as error:
            raise ValueError(f"Invalid camera FOV for {case['name']}") from error
        if not math.isfinite(result) or not 1 <= result <= 170:
            raise ValueError(f"Invalid camera FOV for {case['name']}")
    return result


def capture_motion_pose(case, source_index):
    """Derive the requested pose from accepted source ordinal, including the tail."""
    total = case["frames"] + case.get("count", 1) - 1
    if not 1 <= source_index <= total or total < 2:
        raise ValueError("Camera motion source is outside the requested sequence")
    start, end = case["pose"], case["pose_to"]
    fraction = (source_index - 1) / (total - 1)
    if source_index == 1:
        return start, fraction, total
    if source_index == total:
        return end, fraction, total
    pose = [a + (b - a) * fraction for a, b in zip(start, end)]
    pose[3] = start[3] + math.remainder(end[3] - start[3], 360) * fraction
    return pose, fraction, total


def verify_camera_motion(metadata, case, frames):
    expected_pose, fraction, total = capture_motion_pose(case, frames)
    motion = metadata.get("capture_camera_motion")
    if not isinstance(motion, dict):
        raise ValueError("Capture camera motion metadata is missing or malformed")
    expected = {"schema": 1, "source_index": frames, "total_sources": total,
                "interpolation": "linear world position and pitch; shortest-arc yaw; exact endpoints",
                "clock": "accepted application capture submissions; rejected attempts retain the same pose"}
    for key, value in expected.items():
        if type(motion.get(key)) is not type(value) or motion[key] != value:
            raise ValueError(f"Capture camera motion {key} differs from the requested sequence")
    for key, values in (("start_pose", case["pose"]), ("end_pose", case["pose_to"]),
                        ("current_pose", expected_pose)):
        actual = motion.get(key)
        if (not isinstance(actual, list) or len(actual) != 5 or
                any(type(a) not in (int, float) or not math.isfinite(a) or
                    not math.isclose(a, b, rel_tol=1e-5, abs_tol=1e-5)
                    for a, b in zip(actual, values))):
            raise ValueError(f"Capture camera motion {key} differs from the requested sequence")
    for key, value, tolerance in (("fraction", fraction, 1e-9),
                                  ("vertical_fov", capture_fov(case) or 60, 1e-5)):
        actual = motion.get(key)
        if (type(actual) not in (int, float) or not math.isfinite(actual) or
                not math.isclose(actual, value, rel_tol=tolerance, abs_tol=tolerance)):
            raise ValueError(f"Capture camera motion {key} differs from the requested sequence")
    return expected_pose


def verify_camera(metadata, config, frames, case):
    def number(key, expected):
        actual = metadata.get(key)
        if type(actual) not in (int, float) or not math.isfinite(actual) or not math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-5):
            raise ValueError(f"Capture did not apply {key}={expected!r}: {actual!r}")

    simulation_time = metadata.get("simulation_time")
    if case.get("realtime", False):
        if metadata.get("realtime") is not True or metadata.get("fixed_step") is not None:
            raise ValueError("Capture did not use real-time clocks")
        if type(simulation_time) not in (int, float) or not math.isfinite(simulation_time) or simulation_time <= 0:
            raise ValueError("Invalid real-time simulation timestamp")
    else:
        number("simulation_time", frames / case.get("fps", 60))
        number("fixed_step", 1 / case.get("fps", 60))
        if metadata.get("realtime", False) is not False:
            raise ValueError("Capture unexpectedly used real-time clocks")
    if case.get("count", 1) > 1 or "--capture-floating-motion" in case.get("args", []):
        frozen = "--capture-floating-motion" not in case.get("args", [])
        if metadata.get("floating_islands_frozen") is not frozen:
            raise ValueError("Capture floating-island motion does not match the requested mode")
    if "tour_time" in case:
        duration = metadata.get("tour_duration")
        if type(duration) not in (int, float) or not math.isfinite(duration) or duration < 0 or (duration < case["tour_time"] and not math.isclose(duration, case["tour_time"], rel_tol=1e-6, abs_tol=1e-5)):
            raise ValueError("Capture tour duration is missing or outside the requested start")
        moving = "--capture-tour-motion" in case.get("args", [])
        if metadata.get("tour_motion") is not moving:
            raise ValueError("Capture tour motion does not match the requested mode")
        number("tour_start_time", case["tour_time"])
        number("tour_time", min(case["tour_time"] + (simulation_time if moving else 0), duration))
    else:
        if metadata.get("tour_time") is not None or metadata.get("tour_motion") is not False:
            raise ValueError("Capture unexpectedly used an authored tour")
        if "time_of_day" in config:
            number("time_of_day", config["time_of_day"])
        if "pose" in case:
            pose = (verify_camera_motion(metadata, {**case, "frames": case.get("frames", frames)}, frames)
                    if "pose_to" in case else case["pose"])
            position = metadata.get("camera_position")
            if not isinstance(position, list) or len(position) != 3:
                raise ValueError("Capture camera position is missing or malformed")
            for actual, expected in zip(position, pose[:3]):
                if type(actual) not in (int, float) or not math.isfinite(actual) or not math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-5):
                    raise ValueError("Capture camera position differs from the requested pose")
            number("camera_yaw", pose[3])
            number("camera_pitch", pose[4])
    if "pose_to" not in case and "capture_camera_motion" in metadata:
        raise ValueError("Capture unexpectedly used camera endpoint motion")
    fov = capture_fov(case)
    if fov is not None or "tour_time" not in case:
        number("camera_fov", fov if fov is not None else 60)


def run_capture(command, env, log, timeout):
    process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                               stderr=subprocess.STDOUT, start_new_session=True)
    try:
        return process.wait(timeout=timeout)
    except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        # run.sh currently execs the application. Kill the group as well so a
        # future launcher child cannot survive timeout and overlap later runs.
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        return 130 if isinstance(error, KeyboardInterrupt) else 124


def verify_hdr_pfm(path, width, height):
    """Verify the renderer's unscaled RGB float32 PQ output, in either byte order."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        magic = stream.readline(4096)
        dimensions = stream.readline(4096)
        scale = stream.readline(4096)
        if magic != b"PF\n" or dimensions.split() != [str(width).encode(), str(height).encode()]:
            raise ValueError("HDR PFM header does not match requested RGB image size")
        if scale not in (b"-1.0\n", b"1.0\n"):
            raise ValueError("HDR PFM must contain unscaled float32 PQ values")
        payload_size = width * height * 3 * 4
        if path.stat().st_size != stream.tell() + payload_size:
            raise ValueError("HDR PFM payload is incomplete or has trailing bytes")
        digest.update(magic + dimensions + scale)
        byte_order = "<f" if scale == b"-1.0\n" else ">f"
        consumed = 0
        for block in iter(lambda: stream.read(64 * 1024), b""):
            if len(block) % 4:
                raise ValueError("HDR PFM contains an incomplete float32 sample")
            for (value,) in struct.iter_unpack(byte_order, block):
                if not math.isfinite(value) or not 0.0 <= value <= 1.0:
                    raise ValueError("HDR PFM contains invalid PQ values")
            digest.update(block)
            consumed += len(block)
        if consumed != payload_size:
            raise ValueError("HDR PFM changed during verification")
    return digest.hexdigest()


def capture_outputs(image):
    # Derive paths from the controlled preview name, never from sidecar data.
    return (image, Path(str(image) + ".json"), Path(str(image) + ".config.json"),
            image.with_suffix(".pq.pfm"))


def read_ppm_rgb8(image):
    """Read the renderer's strict P6 RGB8 capture format without image libraries."""
    with image.open("rb") as stream:
        if stream.readline(4096) != b"P6\n":
            raise ValueError("Capture is not binary PPM")
        dimensions = stream.readline(4096).split()
        if len(dimensions) != 2 or stream.readline(4096) != b"255\n":
            raise ValueError("Capture PPM header is not RGB8")
        try:
            width, height = (int(value) for value in dimensions)
        except ValueError as error:
            raise ValueError("Capture PPM dimensions are invalid") from error
        if width <= 0 or height <= 0 or image.stat().st_size != stream.tell() + width * height * 3:
            raise ValueError("Capture PPM payload is incomplete")
        return width, height, stream.read()


def verify_capture(image, config, api, frames, expected_metadata=None, case=None):
    metadata_path = Path(str(image) + ".json")
    effective_path = Path(str(image) + ".config.json")
    metadata = json.loads(metadata_path.read_text())
    effective = json.loads(effective_path.read_text())
    if not isinstance(metadata, dict) or not isinstance(effective, dict):
        raise ValueError("Capture sidecars must be JSON objects")
    for key, expected in (expected_metadata or {}).items():
        actual = metadata
        for part in key.split("."):
            actual = actual.get(part) if isinstance(actual, dict) else None
        if type(expected) in (int, float):
            matches = type(actual) in (int, float) and math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-6)
        else:
            matches = type(actual) is type(expected) and actual == expected
        if not matches:
            raise ValueError(f"Capture did not apply {key}={expected!r}: {actual!r}")
    if api not in str(metadata.get("render_api", "")).lower():
        raise ValueError(f"Requested {api}, captured {metadata.get('render_api')}")
    if metadata.get("benchmark_score") is not False or type(metadata.get("presented_frames")) is not int or metadata["presented_frames"] != frames:
        raise ValueError("Capture frame/benchmark metadata is inconsistent")
    if case is not None:
        verify_fixture_metadata(metadata, case)
        verify_camera(metadata, config, frames, case)
    for key, expected in config.items():
        # Explicit overrides change preset identity; an authored tour changes
        # the time of day. Other serialized controls must actually be applied.
        if key == "quality_preset" or (key == "time_of_day" and (case is None or "tour_time" in case)):
            continue
        actual = effective.get(key)
        if type(expected) in (int, float):
            matches = type(actual) in (int, float) and math.isclose(actual, expected, rel_tol=1e-5, abs_tol=1e-6)
        else:
            matches = type(actual) is type(expected) and actual == expected
        if not matches:
            raise ValueError(f"Effective configuration did not apply {key}={expected!r}: {actual!r}")
    for dimension in ("width", "height"):
        if metadata.get(dimension) != config[dimension]:
            raise ValueError(f"Capture {dimension} differs from requested drawable")
    width, height, _pixels = read_ppm_rgb8(image)
    if width != config["width"] or height != config["height"]:
        raise ValueError("Capture PPM header does not match requested size")
    products = {"image_sha256": sha256(image), "metadata_sha256": sha256(metadata_path),
                "effective_config_sha256": sha256(effective_path), "capture": metadata}
    if effective.get("hdr", False) is True:
        raw_image = image.with_suffix(".pq.pfm")
        declared = metadata.get("hdr_pq_image")
        if not isinstance(declared, str) or Path(declared) != raw_image:
            raise ValueError("HDR metadata must name the capture's sibling PQ PFM")
        products["hdr_pq_sha256"] = verify_hdr_pfm(raw_image, config["width"], config["height"])
    elif "hdr_pq_image" in metadata:
        raise ValueError("SDR capture unexpectedly declares HDR PQ output")
    return products


def write_receipts(path, receipts):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=".results-", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(list(receipts.values()), stream, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def verify_sequence(image, config, api, frames, case):
    # Resolve the CLI default once; later images must retain the same endpoint.
    case = {**case, "frames": frames}
    products = verify_capture(image, config, api, frames, case.get("expected_metadata"), case)
    count = case.get("count", 1)
    if count > 1:
        sequence = []
        previous_time = -1.0
        for index in range(count):
            path = image if index == 0 else image.with_name(f"{image.stem}.{index:06d}{image.suffix}")
            frame = products if index == 0 else verify_capture(
                path, config, api, frames + index, case.get("expected_metadata"), case)
            metadata = frame["capture"]
            if metadata.get("sequence_index") != index or metadata.get("sequence_count") != count:
                raise ValueError("Capture sequence metadata is inconsistent")
            if metadata["simulation_time"] <= previous_time:
                raise ValueError("Capture sequence time did not advance")
            previous_time = metadata["simulation_time"]
            sequence.append({key: value for key, value in frame.items() if key != "capture"} | {"image": str(path)})
        products["sequence"] = sequence
    return products


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("tour", "features", "variants", "all"), default="all")
    parser.add_argument("--manifest", type=Path, help="JSON array of custom cases, same format as --list")
    parser.add_argument("--case", default=".*", help="regular expression matching case names")
    parser.add_argument("--output", type=Path, default=ROOT / "build/visual-audit")
    parser.add_argument("--data", type=Path, default=ROOT / "assets")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/renderer/heaven_benchmark")
    parser.add_argument("--source-root", type=Path,
                        help="Declare fixture catalog/loader snapshot for a frozen renderer; asset inputs are still read from --data")
    parser.add_argument("--api", choices=("vulkan", "opengl"), default="vulkan")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--resume", action="store_true", help="reuse only matching successful receipts")
    args = parser.parse_args()
    if args.source_root is not None:
        try:
            args.source_root = fixture_source_root(args.source_root)
        except ValueError as error:
            parser.error(str(error))
    if args.width <= 0 or args.height <= 0 or not 1 <= args.frames <= 36000 or not 0 < args.timeout < float("inf"):
        parser.error("Dimensions/timeout must be positive and finite; frames must be 1..36000")
    selected = json.loads(args.manifest.read_text()) if args.manifest else cases(args.suite)
    if not isinstance(selected, list):
        parser.error("Case manifest must contain a JSON array")
    for case in selected:
        validate_case(case, args.frames)
    if len({case["name"] for case in selected}) != len(selected):
        parser.error("Case names must be unique")
    selected = [case for case in selected if re.search(args.case, case["name"])]
    if not selected:
        parser.error("No cases selected")
    if args.list:
        print(json.dumps(selected, indent=2))
        return 0
    if os.name != "posix":
        parser.error("This runner uses the Linux run.sh and process-group interface")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    binary_sha = sha256(binary)
    data = args.data.resolve()
    source_data = source_fingerprint(data, selected, args.source_root)
    runtime = runtime_fingerprint()
    receipt_path = output / "results.json"
    previous = json.loads(receipt_path.read_text()) if args.resume and receipt_path.exists() else []
    receipts = {item["case"]["name"]: item for item in previous}
    failures = 0
    for case in selected:
        name = case["name"]
        image = output / f"{name}.ppm"
        log_path = output / f"{name}.log"
        config_path = output / f"{name}.input.json"
        config = {"quality_preset": "balanced", "texture_quality": "original",
                  "music.enabled": False, "width": args.width, "height": args.height,
                  "benchmark_mode": False, "fullscreen": False,
                  **case.get("settings", {})}
        config_path.write_text(json.dumps(config, indent=2) + "\n")
        command = [str(ROOT / "scripts/run.sh"), "--x11" if args.api == "opengl" else "--wayland",
                   "--config", str(config_path), "--data", str(data),
                   "--capture", str(image), "--capture-frames", str(case.get("frames", args.frames))]
        if case.get("count", 1) > 1:
            command += ["--capture-count", str(case["count"])]
        if case.get("realtime", False):
            command += ["--capture-realtime"]
        elif "fps" in case:
            command += ["--capture-fps", str(case["fps"])]
        if "tour_time" in case:
            command += ["--camera-tour-time", str(case["tour_time"])]
        if "pose" in case:
            command += ["--camera-pose", ",".join(str(v) for v in case["pose"])]
        if "pose_to" in case:
            command += ["--capture-camera-to", ",".join(str(v) for v in case["pose_to"])]
        command += case.get("args", [])
        identity = {"case": case, "command": command, "input_config": config,
                    "binary_sha256": binary_sha, "api": args.api,
                    "source_data": source_data, "runtime": runtime}
        frames = case.get("frames", args.frames)
        old = receipts.get(name, {})
        if args.resume and all(old.get(key) == value for key, value in identity.items()) and old.get("success") is True:
            try:
                products = verify_sequence(image, config, args.api, frames, case)
                matching = all(old.get(key) == value for key, value in products.items())
                if matching and log_path.is_file() and sha256(log_path) == old.get("log_sha256"):
                    print(f"Reusing {name}", flush=True)
                    continue
            except (OSError, ValueError, TypeError):
                pass
        # An unsuccessful new run cannot inherit an old screenshot as evidence.
        for product in capture_outputs(image):
            product.unlink(missing_ok=True)
        env = dict(os.environ, HEAVEN_BIN=str(binary), HEAVEN_USER_DIR=str(output / "user"))
        # The wrapper supports environment overrides; the audit's explicit
        # data/config paths must remain authoritative in every child.
        for variable in ("HEAVEN_DATA", "HEAVEN_CONFIG", "HEAVEN_DEFAULT_DATA"):
            env.pop(variable, None)
        if args.api == "opengl":
            env["SDL_VULKAN_LIBRARY"] = "/nonexistent/visual-audit-libvulkan.so"
        start = time.monotonic()
        if config.get("frame_generation", "off") != "off":
            print(f"{name}: real application images only; generated images, provider activation and display cadence need separate native diagnostics", flush=True)
        print(f"Capturing {name}", flush=True)
        with log_path.open("w") as log:
            try:
                code = run_capture(command, env, log, args.timeout)
            except OSError as error:
                log.write(f"Could not start capture: {error}\n")
                code = 127
        entry = {**identity, "exit_code": code, "wall_seconds": time.monotonic() - start}
        try:
            if code != 0:
                raise ValueError(f"Capture process exited {code}")
            entry.update(verify_sequence(image, config, args.api, frames, case))
            entry["success"] = True
        except (OSError, ValueError, TypeError) as error:
            failures += 1
            entry["success"] = False
            entry["error"] = str(error)
        entry["log_sha256"] = sha256(log_path)
        receipts[name] = entry
        write_receipts(receipt_path, receipts)
        print(f"{name}: exit={code}, verified={entry['success']}, {entry['wall_seconds']:.1f}s", flush=True)
        if code == 130:
            return 130
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
