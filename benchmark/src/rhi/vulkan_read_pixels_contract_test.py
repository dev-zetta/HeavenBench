#!/usr/bin/env python3
"""Run the actual-source Vulkan readback host fixture and isolated fail-fast controls."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess


def disable_core():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    receipts = []
    cases = [([], 0, "host contract PASS"),
             (["--negative-host-visibility"], 1, "not made visible to host reads"),
             (["--fatal-wait-oom"], "abort", "vkWaitForFences failed. VkResult=-1"),
             (["--fatal-wait-timeout"], "abort", "vkWaitForFences failed. VkResult=2"),
             (["--fatal-wait-unknown"], "abort", "vkWaitForFences failed. VkResult=-13"),
             (["--fatal-submit-unknown"], "abort", "vkQueueSubmit failed. VkResult=-13")]
    for flags, expected, diagnostic in cases:
        completed = subprocess.run([str(args.executable.resolve()), *flags], capture_output=True,
                                   text=True, timeout=15,
                                   preexec_fn=disable_core if os.name == "posix" else None)
        output = completed.stdout + completed.stderr
        if expected == "abort":
            valid_exit = completed.returncode == -signal.SIGABRT if os.name == "posix" else completed.returncode != 0
        else:
            valid_exit = completed.returncode == expected
        assert valid_exit and diagnostic in output and "unexpectedly returned" not in output, (
            flags, completed.returncode, output)
        receipts.append({"arguments": flags, "returncode": completed.returncode,
                         "expected": expected, "diagnostic": diagnostic,
                         "stdout": completed.stdout, "stderr": completed.stderr})
    result = {"scope": "Actual VulkanReadPixels source with fake native GPU completion and host visibility; no GPU execution",
              "cases": receipts, "passed": True}
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
