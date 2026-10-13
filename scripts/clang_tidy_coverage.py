#!/usr/bin/env python3
"""Fail closed on missing owned translation units and disclose optional coverage."""

import argparse
import json
from pathlib import Path
import sys


# These exact sources depend on the optional FSR SDK/configuration. A new owned
# file is required by default; do not silently skip every file absent from a DB.
FSR_SOURCES = frozenset(Path("benchmark/src/rhi/frame_generation") / name for name in (
    "frame_generation_capture.cpp",
    "frame_generation_capture_contract_test.cpp",
    "frame_generation_platform.cpp",
    "fsr31_pipeline_lifetime_test.cpp",
    "fsr31_view_retirement_test.cpp",
    "fsr31_vulkan.cpp",
    "spirv_reflect_json.cpp",
))
FSR_NATIVE_SOURCE = Path("benchmark/src/rhi/frame_generation/fsr31_vulkan_test.cpp")


def cache_options(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")) or ":" not in line or "=" not in line:
            continue
        name, value = line.split("=", 1)
        result[name.split(":", 1)[0]] = value
    return result


def cmake_enabled(options, name):
    value = options.get(name, "").upper()
    return value not in ("", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND") and not value.endswith("-NOTFOUND")


def check_coverage(root, sources, compile_db, cmake_cache=None):
    entries = json.loads(compile_db.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ValueError("Compilation database must be a JSON array")
    available = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
            raise ValueError("Compilation database entry has no source file")
        path = Path(entry["file"])
        if not path.is_absolute():
            if not isinstance(entry.get("directory"), str):
                raise ValueError("Relative database source has no working directory")
            directory = Path(entry["directory"])
            if not directory.is_absolute():
                directory = compile_db.parent / directory
            path = directory / path
        available.add(path.resolve())
    options = cache_options(cmake_cache) if cmake_cache else {}
    if cmake_cache and "HEAVEN_ENABLE_FSR3" not in options:
        raise ValueError("Renderer cache does not declare HEAVEN_ENABLE_FSR3")
    fsr_enabled = cmake_enabled(options, "HEAVEN_ENABLE_FSR3")
    native_enabled = fsr_enabled and cmake_enabled(options, "HEAVEN_FSR31_NATIVE_TEST")
    selected = []
    unavailable = []
    errors = []
    for source in sources:
        source = source.resolve()
        relative = source.relative_to(root.resolve())
        optional = relative in FSR_SOURCES or relative == FSR_NATIVE_SOURCE
        expected = fsr_enabled if relative in FSR_SOURCES else native_enabled
        if source in available:
            if optional and (not cmake_cache or not expected):
                errors.append(f"{relative.as_posix()}: optional source contradicts CMake configuration")
            else:
                selected.append(relative.as_posix())
        elif optional and cmake_cache and not expected:
            option = "HEAVEN_ENABLE_FSR3" if not fsr_enabled else "HEAVEN_FSR31_NATIVE_TEST"
            unavailable.append({"source": relative.as_posix(), "reason": f"{option} is disabled"})
        else:
            errors.append(f"{relative.as_posix()}: owned source is missing from {compile_db}")
    return {"selected": selected, "unavailable": unavailable, "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--compile-db", type=Path, required=True)
    parser.add_argument("--cmake-cache", type=Path)
    parser.add_argument("sources", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        result = check_coverage(args.root, args.sources, args.compile_db, args.cmake_cache)
    except (OSError, ValueError, TypeError) as error:
        print(f"error: clang-tidy coverage: {error}", file=sys.stderr)
        return 1
    print(f"clang-tidy coverage: {len(result['selected'])} owned sources selected from {args.compile_db}")
    for missing in result["unavailable"]:
        print(f"clang-tidy coverage unavailable: {missing['source']} ({missing['reason']}); audit an enabled build separately")
    for error in result["errors"]:
        print(f"error: clang-tidy coverage: {error}", file=sys.stderr)
    return int(bool(result["errors"]))


if __name__ == "__main__":
    sys.exit(main())
