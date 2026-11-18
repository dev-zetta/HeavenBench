#!/usr/bin/env python3
"""Linux fixed-camera comparison harness with immutable, recursive evidence.

See docs/PROFILING_HARNESS.md. This deliberately does not replace profile.sh's
scored full-tour feature-ablation workflow. No privileged capture is used.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import uuid

from profile_harness.evidence import EvidenceStore
from profile_harness.metrics import analyze_profile, compare_abba, finite_number, relative_range, require
from profile_harness.monitor import file_identity, gpu_observation, host_identity, kernel_interval, processes
from profile_harness.process import run_owned

REPO = Path(__file__).resolve().parents[1]
MAX_OBJECT = 512 * 1024 * 1024
STRIP_PREFIXES = ("VK_", "MESA_", "INTEL_", "ANV_", "HEAVEN_", "SDL_", "RADV_",
                  "GALLIUM_", "DRI_", "__GL_", "MANGOHUD", "RENDERDOC_", "TRACY_",
                  "NSIGHT_", "OPTICK_")
STRIP_KEYS = {"LD_PRELOAD", "LD_LIBRARY_PATH", "LD_DEBUG", "AMD_DEBUG", "ENABLE_VKBASALT"}
COMPARISON_ORDER = ["A1", "B1", "B2", "A2", "A3", "B3", "B4", "A4"]
MONITORING_CONTRACT_FIELDS = ("mode", "clock", "power_limit_w", "device_vram_total_mb",
                              "gpu_high_c", "gpu_critical_c", "vram_high_c", "vram_critical_c")
MONITORING_OBSERVATION_FIELDS = ("sample_monotonic_ns", "available", "reason", "gpu_temperature_c",
                                 "vram_temperature_c", "power_w", "graphics_clock_mhz", "memory_clock_mhz",
                                 "fan_rpm", "utilization_percent", "memory_utilization_percent",
                                 "device_vram_used_mb", "throttle_reason")


def utc():
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f UTC")


def json_read(path):
    return json.loads(Path(path).read_text())


def write_new(path, value):
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def store_open(cohort, create=False):
    return EvidenceStore(cohort / "store", create=create, max_object_bytes=MAX_OBJECT,
                         max_verify_bytes=2 * 1024 * 1024 * 1024)


def seal(store, name, kind, payload, refs=()):
    ref = store.put_record(kind, payload, refs)
    store.verify(ref)
    store.publish(name, ref)
    return ref


def record(store, name):
    ref = store.load(name)
    return ref, store.verify(ref)["payload"]


def copy_blob(store, ref, path, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    store.copy_blob(ref, path)
    if executable:
        path.chmod(0o700)


def snapshot_tree(store, directory):
    files = []
    for path in sorted(directory.rglob("*")):
        require(not path.is_symlink(), f"symlink in cache tree: {path}")
        if path.is_file():
            files.append({"name": path.relative_to(directory).as_posix(), "ref": store.put_file(path)})
    require(len(files) <= 4096, "cache tree file limit exceeded")
    return store.put_record("file-tree", {"files": files}, [item["ref"] for item in files])


def restore_tree(store, tree_ref, destination):
    tree = store.verify(tree_ref)
    require(tree["kind"] == "file-tree", "not a file tree")
    destination.mkdir(parents=True, exist_ok=False)
    for item in tree["payload"]["files"]:
        relative = Path(item["name"])
        require(not relative.is_absolute() and ".." not in relative.parts, "unsafe tree name")
        copy_blob(store, item["ref"], destination / relative)
        (destination / relative).chmod(0o600)


def tree_hashes(directory):
    return [{"name": p.relative_to(directory).as_posix(), **file_identity(p)}
            for p in sorted(directory.rglob("*")) if p.is_file()]


def geometry(text):
    patterns = {
        "base": r"Loaded (\d+) unique meshes, (\d+) instances, (\d+) vertices, (\d+) triangles",
        "tessellated": r"Tessellated geometry: (\d+) vertices, (\d+) triangles \(base: (\d+) vertices, (\d+) triangles\)",
        "prepared": r"Bounded displacement: (\d+) prepared surfaces, (\d+) fallbacks, (\d+) added unique triangles, (\d+) added rendered triangles \(ceiling (\d+)\)",
        "foliage": r"MassiveFoliage buffers: (\d+) source/category owners, (\d+) source ranges, (\d+) patches, (\d+) uploaded vertices / (\d+) instantiated vertices, (\d+) bytes uploaded \((\d+) bytes saved by sharing\)",
        "static_cache": r"CPU tessellation: (\d+) surfaces \((\d+) from cache\)",
        "skinned_cache": r"CPU tessellation \(skinned\): (\d+) surfaces \((\d+) from cache\)"}
    out = {}
    for key, pattern in patterns.items():
        matches = re.findall(pattern, text)
        require(len(matches) == 1, f"missing/ambiguous runtime geometry: {key}")
        out[key] = [int(v) for v in matches[0]]
    return out


def harness_inputs():
    paths = [Path(__file__).resolve(), REPO / "scripts/run.sh", REPO / "scripts/build_paths.sh",
             REPO / 'scripts/profile_power_window.py']
    paths += sorted((REPO / "scripts/profile_harness").glob("*.py"))
    return [file_identity(path) for path in paths]


def native_ownership(pid, native, binary, driver, driver_identity):
    """Bind the observed process image and exact mapped driver inode, not a name."""
    proc = Path(f"/proc/{pid}")
    before = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    if int(before[19]) != native["start_ticks"]:
        return None
    actual, expected = (proc / "exe").stat(), binary.stat()
    if (actual.st_dev, actual.st_ino) != (expected.st_dev, expected.st_ino):
        return None
    expected_map = driver_identity["mapping"]
    matches = []
    for line in (proc / "maps").read_text().splitlines():
        fields = line.split(None, 5)
        if len(fields) == 6 and fields[5] == driver:
            major, minor = (int(p, 16) for p in fields[3].split(":"))
            if (major, minor, int(fields[4])) == (expected_map["device_major"], expected_map["device_minor"], expected_map["inode"]):
                matches.append(fields[:5])
    after = (proc / "stat").read_text().rsplit(")", 1)[1].split()
    if not matches or before[19] != after[19]:
        return None
    return {"native": native, "executable": file_identity(binary),
            "executable_device": actual.st_dev, "executable_inode": actual.st_ino,
            "driver_mappings": matches, "driver": driver_identity}


def prospective_order(spec):
    """An imported cache replaces priming artifacts, never timing windows."""
    imported = "shader_seed_directory" in spec
    require(imported == ("shader_seed_provenance" in spec), "shader seed needs directory and provenance")
    expected = ["pilot-1", "pilot-2"] + COMPARISON_ORDER
    if imported:
        require("prime_seconds" not in spec, "imported shader seed performs no native priming")
    else:
        expected = ["prime-A", "prime-B"] + expected
    require(spec["order"] == expected, "unexpected prospective run order")
    return expected


def validate_hardware_monitoring(spec):
    """Opt-in exact stable contract; absence retains legacy protocol behavior."""
    if "hardware_monitoring" not in spec:
        return
    contract = spec["hardware_monitoring"]
    require(isinstance(contract, dict) and set(contract) == set(MONITORING_CONTRACT_FIELDS),
            "hardware_monitoring needs the exact stable contract fields")
    for key in ("mode", "clock"):
        require(isinstance(contract[key], str) and bool(contract[key].strip()),
                "hardware_monitoring requires nonempty " + key)
    for key in MONITORING_CONTRACT_FIELDS[2:]:
        require(contract[key] is None or (finite_number(contract[key]) and contract[key] >= 0),
                "hardware_monitoring stable metric must be finite nonnegative numeric or null: " + key)


def validate_hardware_observations(observations):
    require(isinstance(observations, dict) and set(observations) == set(MONITORING_OBSERVATION_FIELDS),
            "hardware monitoring observations need the exact measured fields")
    for key, value in observations.items():
        if key == "sample_monotonic_ns":
            require(type(value) is int and 0 <= value < 1 << 64, "invalid hardware monitoring timestamp")
        elif key == "available":
            require(type(value) is bool, "hardware monitoring availability must be boolean")
        elif key in ("reason", "throttle_reason"):
            require(isinstance(value, str) or (key == "throttle_reason" and value is None),
                    "invalid hardware monitoring observation reason")
        else:
            require(value is None or (finite_number(value) and value >= 0),
                    "hardware monitoring observation must be finite nonnegative numeric or null: " + key)


def hardware_monitoring(profile, spec):
    """Separate observed readings without ignoring monitoring conditions."""
    validate_hardware_monitoring(spec)
    actual = profile.get("hardware_telemetry")
    require(isinstance(actual, dict)
            and set(actual) == set(MONITORING_CONTRACT_FIELDS + MONITORING_OBSERVATION_FIELDS),
            "missing or unrecognized hardware monitoring metadata")
    contract = {key: actual[key] for key in MONITORING_CONTRACT_FIELDS}
    validate_hardware_monitoring({"hardware_monitoring": contract})
    require(contract == spec["hardware_monitoring"], "hardware monitoring stable contract differs")
    observations = {key: actual[key] for key in MONITORING_OBSERVATION_FIELDS}
    validate_hardware_observations(observations)
    return contract, observations


def imported_shader_seed(store, spec, cohort, binaries, assets, host, geometry_seed):
    """Verify external qualification, import cache bytes only into this cohort."""
    source = spec["shader_seed_provenance"]
    require(isinstance(source, dict) and set(source) == {"cohort", "entry", "ref", "inventory"},
            "shader seed provenance needs cohort, entry, typed ref and inventory")
    require(source["entry"] == "combined-shader-seed", "only a completed combined shader seed can be imported")
    directory = Path(spec["shader_seed_directory"])
    require(directory.is_dir() and not directory.is_symlink(), "shader seed directory is missing or unsafe")
    require(not cohort.is_relative_to(directory.resolve()) and not directory.resolve().is_relative_to(cohort),
            "shader seed and destination cohort must be separate")
    with store_open(Path(source["cohort"])) as previous:
        seed_ref = previous.load(source["entry"])
        require(seed_ref == source["ref"], "shader seed provenance reference differs")
        seed = previous.verify(seed_ref)
        require(seed["kind"] == "shader-cache-seed" and seed["payload"] == {"producer": "prime-B"}
                and len(seed["refs"]) == 2, "shader seed is not completed native priming")
        tree_ref, window_ref = seed["refs"]
        tree = previous.verify(tree_ref)
        window = previous.verify(window_ref)
        require(tree["kind"] == "file-tree", "shader seed is not a file tree")
        require(window["kind"] == "native-window" and window["payload"].get("id") == "prime-B"
                and window["payload"].get("variant") == "B" and window["payload"].get("passed") is True
                and window["payload"].get("process", {}).get("passed") is True
                and window["payload"].get("process", {}).get("drain", {}).get("passed") is True
                and window["payload"].get("ownership") is not None,
                "shader seed native producer is not qualified")
        old_protocol_ref, old = record(previous, "protocol")
        require(old_protocol_ref in window["refs"] and old["cohort_id"] == window["payload"].get("cohort_id"),
                "shader seed producer is not bound to its protocol")
        require(old["binaries"] == binaries and old["assets"] == assets and old["host"] == host,
                "shader seed binary, assets, hardware or driver differs")
        if "hardware_monitoring" in spec:
            require(old["spec"].get("hardware_monitoring") == spec["hardware_monitoring"]
                    and window["payload"].get("signature", {}).get("hardware_monitoring") == spec["hardware_monitoring"],
                    "shader seed monitoring condition is missing or differs")
        normalize = lambda cfg: {key: value for key, value in cfg.items() if key != "tessellation.cache_file"}
        require(old["geometry_seed"] == geometry_seed
                and normalize(old["expected_config"]) == normalize(json_read(spec["config"]))
                and all(old["spec"][key] == spec[key] for key in ("expected_profile", "expected_geometry", "camera_pose")),
                "shader seed workload, settings or geometry differs")
        files = tree["payload"]["files"]
        require(isinstance(files, list) and 0 < len(files) <= 4096, "empty or oversized shader seed")
        inventory = []
        names = set()
        for item in files:
            name = item["name"]
            require(isinstance(name, str) and name and not Path(name).is_absolute()
                    and all(part not in ("", ".", "..") for part in name.split("/"))
                    and "\\" not in name and name not in names and item["ref"]["format"] == "blob",
                    "unsafe or duplicate shader seed member")
            names.add(name)
            inventory.append({"name": name, "bytes": item["ref"]["bytes"], "sha256": item["ref"]["sha256"]})
        require(inventory == sorted(inventory, key=lambda item: item["name"])
                and source["inventory"] == inventory, "shader seed inventory differs")
    imported_ref = snapshot_tree(store, directory)
    require(imported_ref == tree_ref, "supplied shader cache bytes differ from qualified seed")
    provenance = {"source_cohort": str(Path(source["cohort"]).absolute()),
                  "source_entry": source["entry"], "external_source_ref": seed_ref,
                  "inventory": inventory, "native_priming_performed": False,
                  "scope": "Verified compiler-cache artifact only; no source timing or window is imported or accepted."}
    ref = seal(store, "imported-shader-seed", "imported-shader-cache-seed", provenance, [imported_ref])
    return {"mode": "imported_artifact", "native_priming_planned": False, "seed_ref": ref}, ref


def requested_profile_seconds(spec, label):
    """Validate prospective priming duration; leave measured durations unchanged."""
    prime_seconds = spec.get("prime_seconds", 65)
    profile_seconds = spec["profile_seconds"]
    require(type(prime_seconds) is int and finite_number(profile_seconds)
            and 65 <= prime_seconds <= profile_seconds,
            "prime_seconds must be an integer from 65 through profile_seconds")
    return prime_seconds if label in ("prime-A", "prime-B") else profile_seconds


def prepare(spec_path, cohort):
    spec = json_read(spec_path)
    require(spec["schema"] == 1, "unsupported experiment spec")
    validate_hardware_monitoring(spec)
    prospective_order(spec)
    requested_profile_seconds(spec, "prime-A")
    require(spec["profile_seconds"] >= spec["warmup_seconds"] + spec["window_seconds"] + 2,
            "insufficient profile duration")
    require(spec["max_native_runs"] == len(spec["order"]) + spec.get("prior_native_runs", 0)
            and spec["max_wall_seconds"] > 0,
            "invalid native budget")
    cohort.mkdir(parents=True, exist_ok=False)
    with store_open(cohort, create=True) as store:
        refs, binaries = [], {}
        for variant, item in spec["binaries"].items():
            identity = file_identity(item["path"])
            require(identity["sha256"] == item["sha256"], f"binary pin differs: {variant}")
            ref = store.put_file(item["path"])
            refs.append(ref)
            binaries[variant] = ref
            copy_blob(store, ref, cohort / "subjects" / variant / "heaven_benchmark", executable=True)
        assets = [file_identity(p) for p in spec["assets"]]
        config_ref = store.put_file(spec["config"])
        cache_ref = snapshot_tree(store, Path(spec["geometry_seed"]))
        refs += [config_ref, cache_ref]
        provenance = []
        for path in spec["provenance"]:
            ref = store.put_file(path)
            refs.append(ref)
            provenance.append({"input": file_identity(path), "ref": ref})
        tools = harness_inputs()
        for identity in tools:
            refs.append(store.put_file(identity["path"]))
        host = host_identity(spec["pci"], spec["driver_path"])
        shader_cache = {"mode": "native_priming", "native_priming_planned": True}
        if "shader_seed_directory" in spec:
            shader_cache, imported_ref = imported_shader_seed(store, spec, cohort, binaries, assets, host, cache_ref)
            refs.append(imported_ref)
        protocol = {"schema": 1, "cohort_id": str(uuid.uuid4()), "created_utc": utc(),
                    "spec": spec, "binaries": binaries, "assets": assets,
                    "config": config_ref, "expected_config": json_read(spec["config"]), "geometry_seed": cache_ref,
                    "provenance": provenance, "harness": tools, "shader_cache": shader_cache,
                    "large_input_policy": "Asset ZIPs are externally retained and hash-checked, not embedded; caches/binaries/proofs are embedded.",
                    "host": host}
        seal(store, "protocol", "experiment-protocol", protocol, refs)
        for label in spec["order"]:
            work = cohort / "work" / label
            restore_tree(store, cache_ref, work / "cache")
            (work / "user").mkdir()
            (work / "tmp").mkdir()
            cfg = json_read(spec["config"])
            cfg["tessellation.cache_file"] = str(work / "cache" / spec["neutral_cache_name"])
            write_new(work / "input.config.json", cfg)
    return {"cohort": str(cohort), "prepared": True, "native_runs": 0}


def signature(profile, cfg, spec, text):
    for key, value in spec["expected_profile"].items():
        require(profile.get(key) == value, f"actual profile differs: {key}")
    require(profile.get("width") == cfg["width"] and profile.get("height") == cfg["height"], "drawable mismatch")
    expected = f"GPU selection verified: {spec['gpu_id']} at {spec['pci']}"
    require(expected in text, "actual PCI GPU was not verified")
    for needle in ("Window: SDL video driver = x11", "FilamentBackend: native window = X11",
                   f"Window: drawable size = {cfg['width']}x{cfg['height']}", "Prepared displacement cache: hit"):
        require(needle in text, f"missing native workload evidence: {needle}")
    require("GPU diagnostics requested:" not in text, "GPU diagnostics were active during timing")
    actual_geometry = geometry(text)
    require(actual_geometry == spec["expected_geometry"], "full scene geometry differs")
    normalized = {k: v for k, v in cfg.items() if k != "tessellation.cache_file"}
    keys = ("api", "gpu", "driver", "driver_version", "width", "height", "camera_position",
            "camera_yaw", "camera_pitch", "camera_fov", "render_resolution_scale", "renderer_features", "runtime_settings")
    result = {"profile": {k: profile[k] for k in keys}, "config": normalized,
              "geometry": actual_geometry, "pci": spec["pci"]}
    if "hardware_monitoring" in spec:
        result["hardware_monitoring"], _ = hardware_monitoring(profile, spec)
    return result


def run(cohort, label):
    with store_open(cohort) as store:
        protocol_ref, protocol = record(store, "protocol")
        spec = protocol["spec"]
        prospective_order(spec)
        require(harness_inputs() == protocol["harness"], "harness changed after protocol seal")
        require(host_identity(spec["pci"], spec["driver_path"]) == protocol["host"], "host or driver changed")
        require([file_identity(p) for p in spec["assets"]] == protocol["assets"], "assets changed")
        require(label in spec["order"], "unknown label")
        for name in (f"attempts/{label}", f"windows/{label}"):
            require(not store.exists(name), f"existing immutable run: {name}; use a new cohort")
        index = spec["order"].index(label)
        previous, refs, used_wall = [], [protocol_ref], spec.get("prior_native_wall_seconds", 0.0)
        for prior in spec["order"][:index]:
            ref, payload = record(store, f"windows/{prior}")
            require(payload["passed"], f"prior window failed: {prior}; a new cohort is required")
            require(payload.get("id") == prior and payload.get("cohort_id") == protocol["cohort_id"],
                    "foreign window cannot satisfy this cohort")
            previous.append(payload)
            refs.append(ref)
            used_wall += payload["process"]["elapsed_seconds"]
        require(used_wall + spec["timeout_seconds"] + 15 <= spec["max_wall_seconds"], "native wall budget exhausted (including cleanup reserve)")
        if label in COMPARISON_ORDER:
            pilots = [p["statistics"]["measurement"]["mean_ms"] for p in previous if p["id"].startswith("pilot-")]
            require(len(pilots) == 2 and relative_range(pilots) <= spec["max_baseline_range_percent"], "baseline pilot variance gate failed")
        variant = "B" if label.startswith("B") or label == "prime-B" else "A"
        work = cohort / "work" / label
        binary = cohort / "subjects" / variant / "heaven_benchmark"
        require(file_identity(binary)["sha256"] == protocol["binaries"][variant]["sha256"], "private binary changed")
        if "shader_seed_directory" in spec:
            seed_ref, seed = record(store, "imported-shader-seed")
            require(protocol["shader_cache"] == {"mode": "imported_artifact", "native_priming_planned": False,
                                               "seed_ref": seed_ref}
                    and seed["native_priming_performed"] is False, "imported shader seed policy differs")
            restore_tree(store, store.verify(seed_ref)["refs"][0], work / "cache/xdg")
        elif label == "prime-A":
            (work / "cache/xdg").mkdir(exist_ok=False)
            seed_ref = None
        elif label == "prime-B":
            seed_ref, _ = record(store, "prime-A-shader-seed")
            restore_tree(store, store.verify(seed_ref)["refs"][0], work / "cache/xdg")
        else:
            seed_ref, _ = record(store, "combined-shader-seed")
            restore_tree(store, store.verify(seed_ref)["refs"][0], work / "cache/xdg")
        if seed_ref:
            refs.append(seed_ref)
        cache_policy = {"mode": protocol.get("shader_cache", {}).get("mode", "native_priming"),
                        "new_native_priming_launch": label in ("prime-A", "prime-B"), "seed_ref": seed_ref}
        cfg = json_read(work / "input.config.json")
        expected_cfg = dict(protocol["expected_config"])
        expected_cfg["tessellation.cache_file"] = str(work / "cache" / spec["neutral_cache_name"])
        require(cfg == expected_cfg, "input configuration changed")
        input_ref = store.put_file(work / "input.config.json")
        before_cache = tree_hashes(work / "cache")
        environment = {k: v for k, v in os.environ.items()
                       if k not in STRIP_KEYS and not k.startswith(STRIP_PREFIXES)}
        environment.update(HEAVEN_BIN=str(binary), HEAVEN_DATA=spec["data_dir"],
                           HEAVEN_USER_DIR=str(work / "user"), HEAVEN_DIAGNOSTIC_RUN=str(work),
                           XDG_CACHE_HOME=str(work / "cache/xdg"), TMPDIR=str(work / "tmp"), SDL_VIDEODRIVER="x11")
        seconds = requested_profile_seconds(spec, label)
        argv = ["/usr/bin/stdbuf", "-oL", "-eL", str(REPO / "scripts/run.sh"),
                "--build-dir", str(binary.parent), "--gpu", spec["pci"], "--x11",
                "--config", str(work / "input.config.json"), "--data", spec["data_dir"],
                "--width", str(cfg["width"]), "--height", str(cfg["height"]),
                "--frame-generation", "off", "--vrs", "off", "--no-dynamic-resolution", "--no-music",
                "--camera-fov", "60", "--camera-pose", spec["camera_pose"],
                "--profile", str(work / "profile.json"), "--profile-seconds", str(seconds)]
        before = processes()
        require(not before["blockers"], f"competing native/build processes: {before['blockers']}")
        attempt_ref = seal(store, f"attempts/{label}", "native-attempt", {
            "id": label, "variant": variant, "cohort_id": protocol["cohort_id"],
            "shader_cache": cache_policy,
            "before": before, "gpu_before": gpu_observation(spec["pci"]), "argv": argv,
            "environment_overrides": {k: environment[k] for k in environment if k.startswith(("HEAVEN_", "SDL_", "XDG_CACHE_HOME", "TMPDIR"))},
            "removed_environment_keys": sorted(set(os.environ) - set(environment)),
            "cache_before": before_cache, "used_native_wall_seconds": used_wall}, refs + [input_ref])
        refs += [attempt_ref, input_ref]
        observations, ownership = [], None

        def observe(state):
            nonlocal ownership
            pid = state.get("native", {}).get("pid")
            background = processes(exclude=(os.getpid(), pid))
            require(not background["blockers"], f"competing workload appeared: {background['blockers']}")
            item = {"elapsed_seconds": state["elapsed_seconds"], "gpu": gpu_observation(spec["pci"]),
                    "background": background, "load_average": list(os.getloadavg())}
            if pid and ownership is None:
                try:
                    ownership = native_ownership(pid, state["native"], binary, spec["driver_path"],
                                                 protocol["host"]["vulkan_driver"])
                except (OSError, ValueError):
                    pass
            observations.append(item)
            return {"observation": len(observations) - 1, "native_owner_verified": ownership is not None}

        started = utc()
        try:
            process = run_owned(argv, REPO, environment, work / "process", spec["timeout_seconds"],
                                poll_seconds=2, observer=observe)
        except BaseException as exc:
            failed_refs = refs + [store.put_file(p) for p in (work / "process").glob("*.log")]
            seal(store, f"windows/{label}", "native-controller-failure", {
                "id": label, "variant": variant, "cohort_id": protocol["cohort_id"], "passed": False,
                "shader_cache": cache_policy,
                "error": f"{type(exc).__name__}: {exc}",
                "scope": "Controller failed; no process success, drain or timing qualification claimed."}, failed_refs)
            raise
        finished = utc()
        result = {"id": label, "variant": variant, "cohort_id": protocol["cohort_id"],
                  "shader_cache": cache_policy,
                  "process": process, "passed": False, "started_utc": started, "finished_utc": finished}
        try:
            kernel = kernel_interval(started, finished)
            result["kernel"] = kernel
            require(kernel["available"] and not kernel["fault_lines"], "kernel interval unavailable or contains GPU faults")
            require(process["passed"] and process["drain"]["passed"], "native process failed or did not drain")
            require(ownership is not None, "native executable/Intel driver ownership not observed")
            require(harness_inputs() == protocol["harness"], "harness changed during native run")
            require(host_identity(spec["pci"], spec["driver_path"]) == protocol["host"], "host/driver changed during native run")
            require(file_identity(binary)["sha256"] == protocol["binaries"][variant]["sha256"], "binary changed during run")
            require([file_identity(p) for p in spec["assets"]] == protocol["assets"], "assets changed during run")
            profile = json_read(work / "profile.json")
            effective = json_read(work / "profile.json.config.json")
            require(cfg == effective, "effective configuration differs")
            text = (work / "process/stdout.log").read_text()
            stderr = (work / "process/stderr.log").read_text()
            require(not re.search(r"PanicLog|DEVICE_LOST|VK_ERROR|GPU HANG|Validation Error|AddressSanitizer|Segmentation fault", text + "\n" + stderr, re.I),
                    "application/driver error present in native logs")
            result["signature"] = signature(profile, effective, spec, text)
            if "hardware_monitoring" in spec:
                _, result["hardware_monitoring_observations"] = hardware_monitoring(profile, spec)
            result["statistics"] = analyze_profile(profile, 2 if label.startswith("prime-") else spec["warmup_seconds"],
                                                    60 if label.startswith("prime-") else spec["window_seconds"])
            after_cache = tree_hashes(work / "cache")
            before_geometry = [{k: v for k, v in p.items() if k != "path"} for p in before_cache if not p["name"].startswith("xdg/")]
            after_geometry = [{k: v for k, v in p.items() if k != "path"} for p in after_cache if not p["name"].startswith("xdg/")]
            require(before_geometry == after_geometry, "geometry cache changed")
            result.update(passed=True, ownership=ownership, cache_after=after_cache,
                          shader_cache_changed=[p for p in after_cache if p["name"].startswith("xdg/")] != [p for p in before_cache if p["name"].startswith("xdg/")])
        except Exception as exc:
            result["error"] = f"{type(exc).__name__}: {exc}"
        write_new(work / "observations.json", observations)
        for name in ("observations.json", "input.config.json", "profile.json", "profile.json.config.json",
                     "process/stdout.log", "process/stderr.log"):
            if (work / name).is_file():
                refs.append(store.put_file(work / name))
        ref = seal(store, f"windows/{label}", "native-window", result, refs)
        if result["passed"] and label.startswith("prime-"):
            tree_ref = snapshot_tree(store, work / "cache/xdg")
            seal(store, "prime-A-shader-seed" if label == "prime-A" else "combined-shader-seed",
                 "shader-cache-seed", {"producer": label}, [tree_ref, ref])
        return {"id": label, "passed": result["passed"], "error": result.get("error"),
                "statistics": result.get("statistics"), "native_wall_seconds": process["elapsed_seconds"]}


def analyze(cohort):
    with store_open(cohort) as store:
        protocol_ref, protocol = record(store, "protocol")
        prospective_order(protocol["spec"])
        require(harness_inputs() == protocol["harness"], "analysis harness changed after protocol seal")
        validate_hardware_monitoring(protocol["spec"])
        refs, windows, monitoring_observations = [protocol_ref], [], {}
        for label in protocol["spec"]["order"]:
            ref, payload = record(store, f"windows/{label}")
            require(payload["passed"], f"failed window: {label}")
            require(payload.get("id") == label and payload.get("cohort_id") == protocol["cohort_id"],
                    "foreign window cannot satisfy this cohort")
            refs.append(ref)
            if "hardware_monitoring" in protocol["spec"]:
                require(payload.get("signature", {}).get("hardware_monitoring") == protocol["spec"]["hardware_monitoring"],
                        "missing or foreign hardware monitoring contract")
                observations = payload.get("hardware_monitoring_observations")
                validate_hardware_observations(observations)
                monitoring_observations[label] = observations
            if protocol['spec'].get('power_observer'):
                observer_ref, observer = record(store, f'power_observers/{label}')
                require(observer.get('passed') is True
                        and observer.get('native_window_ref') == ref
                        and observer.get('tool') == protocol['spec']['power_observer']['identity'],
                        'missing or foreign power observer qualification')
                refs.append(observer_ref)
            if label in COMPARISON_ORDER:
                windows.append(payload)
        result = compare_abba(windows, protocol["spec"]["max_baseline_range_percent"])
        result.update(cohort_id=protocol["cohort_id"], measurement_rule={
            "minimum_prefix_seconds": protocol["spec"]["warmup_seconds"],
            "minimum_final_window_seconds": protocol["spec"]["window_seconds"]},
            evidence_verified_recursively=True,
            shader_cache=protocol.get("shader_cache", {"mode": "native_priming", "native_priming_planned": True}))
        if "hardware_monitoring" in protocol["spec"]:
            result["hardware_monitoring"] = {
                "contract": protocol["spec"]["hardware_monitoring"],
                "observations_by_window": monitoring_observations,
                "scope": "Latest host observations, not frame-aligned identity or GPU-pass timing. Full histories remain in each original profile's hardware_telemetry_samples."}
        require(harness_inputs() == protocol["harness"], "analysis harness changed during analysis")
        seal(store, "analysis", "abba-comparison", result, refs)
        write_new(cohort / "report.json", result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("prepare")
    create.add_argument("--spec", required=True, type=Path)
    create.add_argument("--cohort", required=True, type=Path)
    start = commands.add_parser("run")
    start.add_argument("--cohort", required=True, type=Path)
    start.add_argument("--label", required=True)
    summary = commands.add_parser("analyze")
    summary.add_argument("--cohort", required=True, type=Path)
    audit = commands.add_parser("audit")
    audit.add_argument("--cohort", required=True, type=Path)
    audit.add_argument("--entry", default="analysis")
    args = parser.parse_args()
    cohort = args.cohort.resolve()
    if args.command == "prepare":
        result = prepare(args.spec.resolve(), cohort)
    elif args.command == "run":
        result = run(cohort, args.label)
    elif args.command == "analyze":
        result = analyze(cohort)
    else:
        with store_open(cohort) as store:
            ref, payload = record(store, args.entry)
            result = {"verified": True, "entry": args.entry, "ref": ref, "payload": payload}
    print(json.dumps(result, indent=2, allow_nan=False))
    if args.command == "run" and not result["passed"]:
        return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f"profile_compare: {error}", file=sys.stderr)
        sys.exit(2)
