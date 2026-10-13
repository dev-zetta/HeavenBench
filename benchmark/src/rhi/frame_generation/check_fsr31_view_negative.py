#!/usr/bin/env python3
"""Require the actual host executable to fail at the precise unsafe retirement."""
import argparse
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--executable", required=True)
parser.add_argument("--case", choices=("old-double-rotation", "unbounded-inflight"), required=True)
args = parser.parse_args()
expected_record = 2 if args.case == "old-double-rotation" else 4
expected = f"Retired unfinished GPU view: owner record=1 current={expected_record} completed=0"
result = subprocess.run([args.executable, f"--negative-{args.case}"], capture_output=True, text=True, timeout=30)
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
if result.returncode != 1 or result.stdout or result.stderr.strip() != expected:
    raise SystemExit(f"Negative control did not hit its exact lifetime oracle: exit={result.returncode}")
print(f"PASS: {args.case} executable failed at its exact unsafe retirement")
