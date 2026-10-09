#!/usr/bin/env python3
"""Analyze versioned HeavenBench traces without adding CPU and GPU clocks.

Inclusive CPU times overlap by design. Exclusive CPU times subtract the union
of direct same-thread child intervals. Hardware GPU samples and fence estimates
are reported separately; they are never added to CPU time or each other.
"""
from __future__ import annotations

import argparse
import collections
import json
import math
from pathlib import Path
import re
from typing import Any


def integer(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{label} must be a nonnegative integer")
    return value


def identifier(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.isascii() or not value.isdigit():
        raise ValueError(f"{label} must be a decimal uint64 string")
    if int(value) > (1 << 64) - 1:
        raise ValueError(f"{label} exceeds uint64")
    return value


def context(value: Any) -> None:
    if not isinstance(value, dict):
        raise ValueError("context must be an object")
    for key in ("attempt_id", "submission_id", "filament_frame_id", "renderer_id", "view_id", "capture_id"):
        identifier(value[key], key)


def interval(value: dict[str, Any]) -> None:
    if integer(value["end_ns"], "end_ns") < integer(value["begin_ns"], "begin_ns"):
        raise ValueError("reversed interval")


def validate(data: dict[str, Any]) -> dict[str, Any]:
    if data.get("format") != "heaven-microprofile" or data.get("schema") != 1:
        raise ValueError("unsupported microprofile format/schema")
    if data.get("clock") != {"unit": "nanoseconds", "cpu": "steady-clock-relative", "gpu": "frame-relative-uncalibrated"}:
        raise ValueError("unsupported clock contract")
    identifier(data["session_id"], "session_id")
    identifier(data["origin_ns"], "origin_ns")
    interval(data)
    if not isinstance(data["active"], bool):
        raise ValueError("active must be boolean")
    if not isinstance(data["metadata"], dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in data["metadata"].items()):
        raise ValueError("metadata must map strings to strings")
    for key in ("dropped_events", "overwritten_events", "truncated_labels", "memory_bytes"):
        integer(data[key], key)
    integer(data.get("stale_epoch_events", 0), "stale_epoch_events")
    for key in ("threads", "spans", "frames", "counters", "flows", "gpu_samples"):
        if not isinstance(data[key], list):
            raise ValueError(f"{key} must be an array")
    strings = data.get("strings")
    if "strings" in data and (not isinstance(strings, list) or any(not isinstance(s, str) for s in strings)):
        raise ValueError("invalid string table")
    for key in ("threads", "spans", "counters", "flows", "gpu_samples"):
        for record in data[key]:
            for label in ("name", "category", "capability"):
                if label + "_id" not in record:
                    continue
                index = integer(record[label + "_id"], "string reference")
                if strings is None or index >= len(strings) or strings[index] != record[label]:
                    raise ValueError("mismatched string reference")
    for thread in data["threads"]:
        identifier(thread["id"], "thread id")
        if not isinstance(thread["name"], str):
            raise ValueError("thread name must be string")
    keys = set()
    for span in data["spans"]:
        interval(span)
        for key in ("id", "parent_id", "thread_id"):
            identifier(span[key], key)
        context(span["context"])
        if not isinstance(span["name"], str) or not isinstance(span["category"], str) or not isinstance(span["incomplete"], bool):
            raise ValueError("invalid CPU span labels/flag")
        key = (span["thread_id"], span["id"])
        if key in keys:
            raise ValueError("duplicate same-thread CPU span id")
        keys.add(key)
    for frame in data["frames"]:
        interval(frame)
        context(frame["context"])
        if not isinstance(frame["incomplete"], bool):
            raise ValueError("invalid frame incomplete flag")
        if frame["kind"] not in ("Render", "Loading") or frame["outcome"] not in ("Pending", "Accepted", "Rejected", "Cancelled"):
            raise ValueError("invalid frame kind/outcome")
    for counter in data["counters"]:
        integer(counter["timestamp_ns"], "counter timestamp")
        identifier(counter["thread_id"], "counter thread")
        context(counter["context"])
        if not isinstance(counter["name"], str) or not isinstance(counter["category"], str):
            raise ValueError("invalid counter labels")
        if isinstance(counter["value"], bool) or not isinstance(counter["value"], (int, float)) or not math.isfinite(counter["value"]):
            raise ValueError("nonfinite/invalid counter")
    for flow in data["flows"]:
        integer(flow["timestamp_ns"], "flow timestamp")
        identifier(flow["id"], "flow id")
        identifier(flow["thread_id"], "flow thread")
        context(flow["context"])
        if flow["phase"] not in ("Begin", "Step", "End"):
            raise ValueError("invalid flow phase")
    for sample in data["gpu_samples"]:
        context(sample["context"])
        identifier(sample["pass_id"], "GPU pass")
        identifier(sample["parent_pass_id"], "GPU parent")
        for key in ("command_count", "primitive_count", "visible_renderables"):
            identifier(sample[key], key)
        identifier(sample.get("draw_instance_invocations", "0"), "draw_instance_invocations")
        if sample["status"] not in ("Available", "Pending", "Unsupported", "Disjoint", "Error", "Dropped"):
            raise ValueError("invalid GPU status")
        if sample["source"] not in ("HardwareTimestamp", "FenceEstimate", "Unavailable"):
            raise ValueError("invalid GPU source")
        if sample["status"] == "Available":
            interval(sample)
    return data


def union_length(intervals: list[tuple[int, int]]) -> int:
    total, end = 0, -1
    for start, stop in sorted(intervals):
        if stop > end:
            total += stop - max(start, end)
            end = stop
    return total


def distribution(samples: list[int | float]) -> dict[str, Any] | None:
    if not samples:
        return None
    ordered = sorted(samples)
    def quantile(fraction: float) -> float:
        position = (len(ordered) - 1) * fraction
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return {"count": len(ordered), "min": ordered[0], "mean": sum(ordered) / len(ordered),
            "p50": quantile(.50), "p95": quantile(.95), "p99": quantile(.99), "max": ordered[-1]}


def analyze(data: dict[str, Any]) -> dict[str, Any]:
    validate(data)
    children: dict[tuple[str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for span in data["spans"]:
        if span["parent_id"] != "0":
            children[(span["thread_id"], span["parent_id"])].append(span)
    cpu: dict[tuple[str, str, str], dict[str, Any]] = {}
    for span in data["spans"]:
        key = (span["thread_id"], span["category"], span["name"])
        row = cpu.setdefault(key, {"thread_id": key[0], "category": key[1], "name": key[2], "count": 0,
                                   "incomplete_count": 0, "inclusive_ns": 0, "exclusive_ns": 0, "exclusive_valid_count": 0, "exclusive_unavailable_count": 0, "max_ns": 0, "_inclusive": [], "_exclusive": []})
        if span["incomplete"]:
            row["incomplete_count"] += 1
            continue
        duration = span["end_ns"] - span["begin_ns"]
        subtract = []
        exclusive_valid = not (data["dropped_events"] or data["overwritten_events"])
        for child in children[(span["thread_id"], span["id"])]:
            if child["incomplete"] or child["begin_ns"] < span["begin_ns"] or child["end_ns"] > span["end_ns"]:
                exclusive_valid = False
            start, stop = max(span["begin_ns"], child["begin_ns"]), min(span["end_ns"], child["end_ns"])
            if stop > start:
                subtract.append((start, stop))
        row["count"] += 1
        row["inclusive_ns"] += duration
        row["_inclusive"].append(duration)
        if exclusive_valid:
            row["exclusive_valid_count"] += 1
            row["_exclusive"].append(duration - union_length(subtract))
            if row["exclusive_ns"] is not None:
                row["exclusive_ns"] += duration - union_length(subtract)
        else:
            row["exclusive_unavailable_count"] += 1
            row["exclusive_ns"] = None
        row["max_ns"] = max(row["max_ns"], duration)
    gpu: dict[tuple[str, str], dict[str, Any]] = {}
    statuses: collections.Counter[str] = collections.Counter()
    for sample in data["gpu_samples"]:
        statuses[sample["status"]] += 1
        if sample["status"] != "Available":
            continue
        key = (sample["source"], sample["name"])
        row = gpu.setdefault(key, {"source": key[0], "name": key[1], "count": 0, "inclusive_ns": 0, "max_ns": 0, "_samples": []})
        duration = sample["end_ns"] - sample["begin_ns"]
        row["count"] += 1
        row["inclusive_ns"] += duration
        row["max_ns"] = max(row["max_ns"], duration)
        row["_samples"].append(duration)
    for row in cpu.values():
        row["inclusive_distribution_ns"] = distribution(row.pop("_inclusive"))
        valid = row.pop("_exclusive")
        row["exclusive_distribution_ns"] = distribution(valid) if row["exclusive_ns"] is not None else None
    for row in gpu.values():
        row["inclusive_distribution_ns"] = distribution(row.pop("_samples"))
    counters: dict[tuple[str, str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for sample in data["counters"]:
        counters[(sample["thread_id"], sample["category"], sample["name"])].append(sample)
    counter_rows = []
    for key, samples in sorted(counters.items()):
        samples.sort(key=lambda s: s["timestamp_ns"])
        counter_rows.append({"thread_id": key[0], "category": key[1], "name": key[2],
                             "distribution": distribution([s["value"] for s in samples]),
                             "first": {k: samples[0][k] for k in ("timestamp_ns", "value", "context")},
                             "last": {k: samples[-1][k] for k in ("timestamp_ns", "value", "context")}})
    workloads: dict[tuple[str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for sample in data["gpu_samples"]:
        workloads[(sample["source"], sample["name"])].append(sample)
    workload_rows = [{"source": key[0], "name": key[1], "count": len(samples),
                      "statuses": dict(collections.Counter(s["status"] for s in samples)),
                      **{field: distribution([int(s[field]) for s in samples if field in s])
                         for field in ("command_count", "primitive_count", "visible_renderables", "draw_instance_invocations")}}
                     for key, samples in sorted(workloads.items())]
    # Per-pass GPU inclusive times can overlap (nested passes/multiple queues).
    # No aggregate total is inferred from their sum.
    frames = collections.Counter(f["kind"] + ":" + f["outcome"] + (":Incomplete" if f["incomplete"] else "") for f in data["frames"])
    complete_frames = [f for f in data["frames"] if f["kind"] == "Render" and f["outcome"] == "Accepted"
                       and not f["incomplete"] and data["begin_ns"] <= f["begin_ns"] <= f["end_ns"] <= data["end_ns"]]
    worst_frames = [{**f, "duration_ns": f["end_ns"] - f["begin_ns"]}
                    for f in sorted(complete_frames, key=lambda f: -(f["end_ns"] - f["begin_ns"]))[:20]]
    # A union of observed scopes is wall coverage, not thread CPU utilization:
    # scoped waits count, uninstrumented work is unknown, and workers overlap.
    thread_names = {t["id"]: t["name"] for t in data["threads"]}
    thread_spans: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for span in data["spans"]:
        thread_spans[span["thread_id"]].append(span)
    activity = []
    window = data["end_ns"] - data["begin_ns"]
    for thread_id in sorted(thread_names.keys() | thread_spans.keys(), key=int):
        spans = thread_spans[thread_id]
        clipped = sum(s["begin_ns"] < data["begin_ns"] or s["end_ns"] > data["end_ns"] for s in spans)
        incomplete = sum(s["incomplete"] for s in spans)
        intervals = [(max(s["begin_ns"], data["begin_ns"]), min(s["end_ns"], data["end_ns"])) for s in spans]
        observed = union_length([(start, stop) for start, stop in intervals if stop > start])
        activity.append({"thread_id": thread_id, "name": thread_names.get(thread_id, ""),
                         "observed_active_ns": observed, "observed_wall_fraction": observed / window if window else None,
                         "complete_span_count": len(spans) - incomplete, "incomplete_span_count": incomplete,
                         "window_clipped_span_count": clipped,
                         "coverage_incomplete": bool(data["dropped_events"] or data["overwritten_events"] or incomplete or clipped)})
    return {"schema": 1, "session_id": data["session_id"], "metadata": data["metadata"],
            "window_ns": data["end_ns"] - data["begin_ns"], "frames": dict(frames),
            "accepted_complete_cpu_frame_duration_ns": distribution([f["end_ns"] - f["begin_ns"] for f in complete_frames]),
            "worst_frames": worst_frames, "thread_activity": activity,
            "quantiles": "Linear interpolation between ordered samples (R7); timings use nanoseconds, counters/workloads retain recorded units.",
            "loss": {k: data[k] for k in ("dropped_events", "overwritten_events", "truncated_labels")},
            "stale_epoch_events": data.get("stale_epoch_events", 0),
            "cpu": sorted(cpu.values(), key=lambda r: -(r["exclusive_ns"] or 0)),
            "gpu": sorted(gpu.values(), key=lambda r: -r["inclusive_ns"]), "gpu_statuses": dict(statuses),
            "counters": counter_rows, "gpu_workloads": workload_rows,
            "limits": ["CPU inclusive times overlap; exclusive is same-thread direct-child interval union subtraction.",
                       "Incomplete CPU spans are counted separately and excluded from complete-span totals.",
                       "Exclusive cost is unavailable on event loss or incomplete/out-of-parent children; it is never guessed.",
                       "Stale-epoch imports are known foreign-capture events, reported separately from current event loss.",
                       "GPU samples use their own frame-relative clock; hardware/fence results remain separate.",
                       "Per-pass GPU inclusive times overlap and are not a frame total; CPU and GPU are not added.",
                       "Counters are observations with unknown gauge/delta semantics; values are not summed.",
                       "GPU workload distributions include all retained pass statuses, can overlap, and are not frame totals.",
                       "Draw-instance invocations sum instances over executed draws, not unique visible placements.",
                       "Absent legacy workload fields have no observed distribution; absence is not a measured zero.",
                       "Worst frames are at most 20 retained complete accepted Render CPU intervals; missing history cannot be ranked.",
                       "Thread activity is an observed same-thread scope union/window fraction, not CPU utilization; waits and nested tasks count once, and uninstrumented work is unknown.",
                       "Thread coverage is marked incomplete on event loss or partial/clipped spans; threads are never summed as wall time.",
                       "Recorded frames are attempts/submissions, not hardware presentation timing or benchmark scores."]}


def compare(left: dict[str, Any], right: dict[str, Any]) -> dict[str, Any]:
    a, b = analyze(left), analyze(right)
    if a["metadata"].get("settings_mixed") == "true" or b["metadata"].get("settings_mixed") == "true":
        raise ValueError("comparison refuses captures with mixed settings")
    required = ("cpu", "gpu", "driver", "driver_version", "api", "resolution", "effective_settings")
    invalid = []
    for side, summary in (("a", a), ("b", b)):
        for key in required:
            value = summary["metadata"].get(key, "").strip().casefold().strip("() ")
            if value in ("", "none", "null", "n/a", "na", "-") or re.match(
                    r"^(unknown|unavailable|unsupported|not available|not detected|not reported)\b", value):
                invalid.append(f"{side}.{key}")
    if invalid:
        raise ValueError("comparison requires known nonempty identity metadata; missing/unknown fields: " + ", ".join(invalid))
    if not a["metadata"] or a["metadata"] != b["metadata"]:
        raise ValueError("comparison requires identical nonempty recorded metadata")
    def rows(summary: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
        # Thread IDs may change between processes; named scopes aggregate only
        # for comparison. Retain per-thread identities in each single report.
        out: dict[tuple[str, str], dict[str, Any]] = {}
        for r in summary["cpu"]:
            if r["exclusive_ns"] is None:
                continue
            key = (r["category"], r["name"])
            target = out.setdefault(key, {"count": 0, "exclusive_ns": 0})
            target["count"] += r["count"]
            target["exclusive_ns"] += r["exclusive_ns"]
        return out
    ar, br = rows(a), rows(b)
    changes = []
    for key in sorted(ar.keys() & br.keys()):
        if not ar[key]["count"] or not br[key]["count"]:
            continue
        av, bv = ar[key]["exclusive_ns"] / ar[key]["count"], br[key]["exclusive_ns"] / br[key]["count"]
        changes.append({"category": key[0], "name": key[1], "a_mean_exclusive_ns": av,
                        "b_mean_exclusive_ns": bv, "count_a": ar[key]["count"], "count_b": br[key]["count"],
                        "change_percent": (bv / av - 1) * 100 if av else None})
    return {"metadata": a["metadata"], "cpu_mean_per_complete_scope": changes,
            "counters_a": a["counters"], "counters_b": b["counters"],
            "gpu_workloads_a": a["gpu_workloads"], "gpu_workloads_b": b["gpu_workloads"],
            "worst_frames_a": a["worst_frames"], "worst_frames_b": b["worst_frames"],
            "thread_activity_a": a["thread_activity"], "thread_activity_b": b["thread_activity"],
            "stale_epoch_events_a": a["stale_epoch_events"], "stale_epoch_events_b": b["stale_epoch_events"],
            "loss_a": a["loss"], "loss_b": b["loss"], "qualification": "Diagnostic scope comparison; unequal workload counts and event loss remain visible, not a score. Matching recorded metadata does not independently verify executable or scene-input identity."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--output", type=Path, help="Write analysis JSON instead of stdout")
    args = parser.parse_args()
    try:
        data = json.loads(args.trace.read_text(encoding="utf-8"))
        result = compare(data, json.loads(args.compare.read_text(encoding="utf-8"))) if args.compare else analyze(data)
        text = json.dumps(result, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8", newline="\n")
        else:
            print(text, end="")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"microprofile analysis failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
