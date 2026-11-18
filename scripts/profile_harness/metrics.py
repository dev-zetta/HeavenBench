"""Strict, CPU-only analysis of native fixed-camera frame profiles.

Intervals crossing a boundary are included whole. GPU completion samples can
lag/repeat; they are deliberately not treated as aligned per-frame GPU costs.
"""
import math
import statistics
import struct

COLUMNS = ["presented_interval_seconds", "camera_ms", "scene_sync_ms",
           "features_ms", "render_submit_ms", "last_completed_gpu_ms"]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite_number(value):
    return (not isinstance(value, bool) and isinstance(value, (int, float))
            and math.isfinite(value))


def summary(intervals):
    require(bool(intervals), "empty measurement window")
    seconds = math.fsum(intervals)
    ordered = sorted(intervals)
    result = {"frames": len(intervals), "seconds": seconds,
              "mean_ms": seconds * 1000 / len(intervals),
              "fps": len(intervals) / seconds}
    for p in (50, 95, 99):
        result[f"p{p}_ms"] = ordered[math.ceil(len(ordered) * p / 100) - 1] * 1000
    return result


def analyze_profile(data, warmup_seconds=60.0, window_seconds=120.0):
    require(type(data.get("schema")) is int and data.get("schema") == 1 and data.get("columns") == COLUMNS,
            "unsupported native profile schema/columns")
    for key in ("benchmark_score", "screenshot_readback", "overlay_visible",
                "floating_islands_frozen"):
        require(data.get(key) is False, f"unexpected capture mode: {key}")
    require(finite_number(warmup_seconds) and warmup_seconds >= 2,
            "invalid warmup duration")
    require(finite_number(window_seconds) and window_seconds >= 60,
            "invalid measurement duration")
    rows = data.get("samples_including_warmup")
    require(isinstance(rows, list) and bool(rows), "missing native samples")
    for row in rows:
        require(isinstance(row, list) and len(row) == len(COLUMNS)
                and all(finite_number(v) for v in row), "invalid native sample")
    try:
        intervals = [struct.unpack("f", struct.pack("f", r[0]))[0] for r in rows]
    except (OverflowError, struct.error) as exc:
        raise ValueError("invalid float32 interval") from exc
    require(all(math.isfinite(v) and v > 0 for v in intervals), "nonpositive interval")
    native_start, elapsed = 0, 0.0
    while native_start < len(intervals) and elapsed < 2.0:
        elapsed += intervals[native_start]
        native_start += 1
    native = summary(intervals[native_start:])
    require(type(data.get("measured_frames")) is int and data.get("measured_frames") == native["frames"], "native frame count mismatch")
    require(finite_number(data.get("average_fps"))
            and math.isclose(data["average_fps"], native["fps"], rel_tol=2e-6),
            "native FPS mismatch")
    require(finite_number(data.get("measured_seconds"))
            and math.isclose(data["measured_seconds"], native["seconds"], rel_tol=2e-6),
            "native duration mismatch")
    require(finite_number(data.get("warmup_seconds"))
            and math.isclose(data["warmup_seconds"], elapsed, rel_tol=2e-6),
            "native warmup mismatch")
    start, duration = len(intervals), 0.0
    while start and duration < window_seconds:
        start -= 1
        duration += intervals[start]
    prefix = math.fsum(intervals[:start])
    require(duration >= window_seconds and prefix >= warmup_seconds and start >= native_start,
            "profile too short for declared warmup and final measurement window")
    measured = summary(intervals[start:])
    return {"native": native, "measurement": measured,
            "selection": {"rule": "shortest final whole-interval suffix",
                          "start_index": start, "end_index": len(intervals),
                          "excluded_prefix_seconds": prefix,
                          "minimum_prefix_seconds": warmup_seconds,
                          "minimum_window_seconds": window_seconds},
            "gpu_note": "Last-completed GPU samples are asynchronous; no CPU/GPU sum or alignment inferred."}


def relative_range(values):
    require(bool(values) and all(finite_number(v) and v > 0 for v in values),
            "invalid cohort values")
    return (max(values) - min(values)) / statistics.mean(values) * 100


def compare_abba(windows, max_baseline_range_percent=1.0):
    """Require two complete quartets; pooled per-frame means, no rejected trimming."""
    require(finite_number(max_baseline_range_percent) and max_baseline_range_percent >= 0,
            "invalid baseline repeatability bound")
    require([w["variant"] for w in windows] == list("ABBAABBA"),
            "acceptance requires two complete ordered ABBA quartets")
    require(len({w["id"] for w in windows}) == 8, "duplicate window identity")
    signatures = [w["signature"] for w in windows]
    require(all(s == signatures[0] for s in signatures), "workload/hardware mismatch")
    for w in windows:
        m = w["statistics"]["measurement"]
        require(type(m["frames"]) is int and m["frames"] > 0
                and all(finite_number(m[k]) and m[k] > 0 for k in ("seconds", "mean_ms", "fps")),
                "invalid measurement statistics")
        require(math.isclose(m["mean_ms"], m["seconds"] * 1000 / m["frames"], rel_tol=1e-12)
                and math.isclose(m["fps"], m["frames"] / m["seconds"], rel_tol=1e-12),
                "inconsistent measurement statistics")
    results = {}
    for variant in ("A", "B"):
        samples = [w["statistics"]["measurement"] for w in windows if w["variant"] == variant]
        means = [s["mean_ms"] for s in samples]
        mean = math.fsum(s["seconds"] for s in samples) * 1000 / sum(s["frames"] for s in samples)
        results[variant] = {"mean_ms": mean, "fps": 1000 / mean,
                            "range_percent": relative_range(means), "windows_ms": means}
    gain = (1 - results["B"]["mean_ms"] / results["A"]["mean_ms"]) * 100
    a = results["A"]["windows_ms"]
    drift = abs(a[-1] - a[0]) / statistics.mean(a) * 100
    gate = max(results["A"]["range_percent"], results["B"]["range_percent"], drift)
    quartet_gains = []
    for begin in (0, 4):
        group = windows[begin:begin + 4]
        mean = {}
        for variant in ("A", "B"):
            m = [w["statistics"]["measurement"] for w in group if w["variant"] == variant]
            mean[variant] = math.fsum(s["seconds"] for s in m) / sum(s["frames"] for s in m)
        quartet_gains.append((1 - mean["B"] / mean["A"]) * 100)
    return {"variants": results, "improvement_percent": gain,
            "baseline_first_last_drift_percent": drift, "noise_gate_percent": gate,
            "quartet_improvements_percent": quartet_gains,
            "max_baseline_range_percent": max_baseline_range_percent,
            "passed": (results["A"]["range_percent"] <= max_baseline_range_percent
                       and gain > gate and all(g > 0 for g in quartet_gains))}
