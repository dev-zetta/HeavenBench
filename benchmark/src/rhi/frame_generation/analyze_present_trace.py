#!/usr/bin/env python3
"""Analyze bounded KHR present-completion observations, never physical display FPS."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import statistics


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    return {"count": len(values), "mean_ms": statistics.mean(values) / 1e6,
            "p50_ms": values[(len(values) - 1) // 2] / 1e6,
            "p95_ms": values[int((len(values) - 1) * 0.95)] / 1e6,
            "max_ms": values[-1] / 1e6}


def validate_samples(samples):
    statuses = {"pending", "khr_complete", "unsupported", "queue_failed", "wait_failed",
                "pending_overflow", "unresolved_timeout", "unresolved_retired"}
    last_id = 0
    previous_by_chain = {}
    matched_pairs = 0
    generated = 0
    for sample in samples:
        for field in ("chain", "present_id", "source_frame", "submit_begin_ns", "submit_end_ns"):
            if type(sample[field]) is not int or sample[field] < 0:
                raise ValueError("Invalid nonnegative integer identity/time: " + field)
        # The producer uses one monotonically increasing counter across chains.
        # Check retained submission order, including failures/unresolved samples;
        # sorting completed IDs would hide duplicate or reversed records.
        if sample["present_id"]:
            if sample["present_id"] <= last_id:
                raise ValueError("Duplicate/nonmonotonic present ID")
            last_id = sample["present_id"]
            if sample["chain"] == 0:
                raise ValueError("Present ID has no native chain identity")
        elif sample["status"] not in ("unsupported", "queue_failed"):
            raise ValueError("Waitable observation has no present ID")
        if sample["status"] not in statuses:
            raise ValueError("Unknown presentation observation status")
        if sample["kind"] not in ("real", "generated", "unattributed"):
            raise ValueError("Unknown presentation source kind")
        if (sample["kind"] == "unattributed") != (sample["source_frame"] == 0):
            raise ValueError("Presentation kind/source identity mismatch")
        if sample["submit_end_ns"] < sample["submit_begin_ns"]:
            raise ValueError("Invalid host submission interval")
        lower, upper = sample["khr_signal_lower_ns"], sample["khr_signal_upper_ns"]
        if lower is not None and (type(lower) is not int or lower < sample["submit_begin_ns"]):
            raise ValueError("Invalid KHR signal lower bound")
        if sample["status"] == "khr_complete":
            if lower is None or type(upper) is not int or upper < max(lower, sample["submit_end_ns"]):
                raise ValueError("Invalid KHR signal bounds")
        elif upper is not None:
            raise ValueError("Incomplete timing invented a completion upper bound")
        if sample["status"] in ("unsupported", "queue_failed") and lower is not None:
            raise ValueError("Unwaited observation invented a completion lower bound")
        previous = previous_by_chain.get(sample["chain"])
        if previous and previous["kind"] == "generated" and previous["queue_result"] in (0, 1000001003):
            if previous["present_id"] and sample["present_id"] == previous["present_id"] + 1:
                if sample["kind"] != "real" or sample["source_frame"] != previous["source_frame"]:
                    raise ValueError("Generated/restored-real source identity mismatch")
                matched_pairs += 1
        generated += sample["kind"] == "generated"
        previous_by_chain[sample["chain"]] = sample
    return {"generated_records": generated, "immediate_generated_real_pairs": matched_pairs,
            "generated_without_retained_immediate_real": generated - matched_pairs,
            "scope": "Pair checks require consecutive retained IDs after an accepted generated queue call. "
                     "Missing/failed records do not prove a source mismatch. Runtime reload may reset source_frame; "
                     "chain/present_id remain the presentation identities."}


def intervals_for_completed(samples):
    chains = defaultdict(list)
    for sample in samples:
        if sample["status"] == "khr_complete":
            chains[sample["chain"]].append(sample)
    interval_bounds = []
    for chain, records in chains.items():
        for previous, current in zip(records, records[1:]):
            lower = max(0, current["khr_signal_lower_ns"] - previous["khr_signal_upper_ns"])
            upper = max(0, current["khr_signal_upper_ns"] - previous["khr_signal_lower_ns"])
            interval_bounds.append({"chain": chain, "from_id": previous["present_id"],
                "to_id": current["present_id"], "from_kind": previous["kind"], "to_kind": current["kind"],
                "from_source_frame": previous["source_frame"], "to_source_frame": current["source_frame"],
                "same_source": previous["source_frame"] != 0 and previous["source_frame"] == current["source_frame"],
                "consecutive_retained_ids": current["present_id"] == previous["present_id"] + 1,
                "queue_call_interval_ns": current["submit_begin_ns"] - previous["submit_begin_ns"],
                "lower_ns": lower, "upper_ns": upper,
                "lower_ms": lower / 1e6, "upper_ms": upper / 1e6})
    return interval_bounds


def interval_statistics(intervals):
    return {"interval_count": len(intervals),
            "consecutive_retained_id_intervals": sum(i["consecutive_retained_ids"] for i in intervals),
            "intervals_spanning_other_or_missing_ids": sum(not i["consecutive_retained_ids"] for i in intervals),
            "queue_call_intervals_for_completed_ids": distribution([i["queue_call_interval_ns"] for i in intervals]),
            "khr_signal_interval_lower_bounds": distribution([i["lower_ns"] for i in intervals]),
            "khr_signal_interval_upper_bounds": distribution([i["upper_ns"] for i in intervals])}


def sample_statistics(samples, intervals):
    completed = [s for s in samples if s["status"] == "khr_complete"]
    return {"retained_samples": len(samples),
            "status_counts": dict(Counter(sample["status"] for sample in samples)),
            "kind_counts": dict(Counter(sample["kind"] for sample in samples)),
            "actual_present_modes": sorted({sample["present_mode_label"] for sample in samples
                                             if sample["present_mode_label"] != "unknown"}),
            "unknown_present_mode_samples": sum(sample["present_mode_label"] == "unknown" for sample in samples),
            "completion_bound_widths": distribution([s["khr_signal_upper_ns"] - s["khr_signal_lower_ns"]
                                                       for s in completed]),
            **interval_statistics(intervals)}


def transition_statistics(intervals):
    transitions = defaultdict(list)
    for interval in intervals:
        transitions[interval["from_kind"] + "->" + interval["to_kind"]].append(interval)
    return {key: interval_statistics(rows) for key, rows in sorted(transitions.items())}


def scope_statistics(samples):
    intervals = intervals_for_completed(samples)
    kinds, chains = defaultdict(list), defaultdict(list)
    for sample in samples:
        kinds[sample["kind"]].append(sample)
        chains[sample["chain"]].append(sample)
    by_chain = {}
    for chain, rows in sorted(chains.items()):
        chain_intervals = intervals_for_completed(rows)
        by_chain[str(chain)] = {**sample_statistics(rows, chain_intervals),
                                "transitions": transition_statistics(chain_intervals)}
    return {**sample_statistics(samples, intervals), "khr_signal_interval_bounds": intervals,
            "by_kind": {kind: sample_statistics(rows, intervals_for_completed(rows)) for kind, rows in sorted(kinds.items())},
            "by_chain": by_chain, "transitions": transition_statistics(intervals)}


def analyze(document):
    if document.get("format") != "heaven-present-trace" or document.get("schema") != 1:
        raise ValueError("Unsupported presentation trace format/schema")
    samples = document["samples"]
    if len(samples) > 32768:
        raise ValueError("Trace exceeds its sample bound")
    pairing = validate_samples(samples)
    all_statistics = scope_statistics(samples)
    return {"format": "heaven-present-analysis", "schema": 1,
            "available": document["available"], "reason": document["reason"],
            "scope": "All retained observations; use attributed_runtime for the accepted application sources.",
            "dropped_samples": document["dropped_samples"], **all_statistics,
            "attributed_runtime": scope_statistics([s for s in samples if s["kind"] != "unattributed"]),
            "unattributed": scope_statistics([s for s in samples if s["kind"] == "unattributed"]),
            "source_pair_coverage": pairing,
            "limitations": document["semantics"] +
                " Interval bounds can overlap or span missing observations; counts are not confirmed displayed images. "
                "Kind/chain subsets can span intervening kinds, missing IDs or reloads, as flagged. "
                "Unattributed observations include loading/startup but are not classified as startup by inference. "
                "Trace-mode observer/export overhead is diagnostic and requires an independent off/on cost comparison."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps(analyze(json.loads(args.trace.read_text())), indent=2) + "\n"
    if args.output:
        args.output.write_text(report)
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
