#!/usr/bin/env python3
"""Collect release license texts with repository/dependency provenance.

Usage: collect_licenses.py --output DIR [--dependency-dir [NAME=]DIR ...]
External dependency directories should contain installed license notices (for
example a vcpkg triplet's share directory or a system package's doc directory).
This scans license filenames, not source trees for arbitrary copyright claims.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
PRUNE = {".git", "build", "builds", "buildtrees", "test", "tests", "test_images",
         "samples", "examples", "assets", "art", "docs", "docs_src", "web", "node_modules"}
LICENSE_NAME = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)([._-].*)?$", re.I)
VENDORS = ("filament", "JoltPhysics", "OpenSubdiv", "libzip", "pocketfft", "bcdec", "rapidcheck")


def license_files(directory):
    for current, directories, files in os.walk(directory, followlinks=False):
        directories[:] = sorted(name for name in directories
                                if name.lower() not in PRUNE
                                and not (Path(current) / name).is_symlink())
        for name in sorted(files):
            path = Path(current) / name
            if LICENSE_NAME.fullmatch(name) and path.is_file():
                yield path


def leading_comment(path):
    text = path.read_text(encoding="utf-8-sig")
    for match in re.finditer(r"/\*.*?\*/", text[:16384], re.S):
        if "copyright" in match.group().lower():
            return (match.group() + "\n").encode()
    raise ValueError(f"Expected copyright notice in {path}")


def collect(output, dependency_dirs):
    output.mkdir(parents=True, exist_ok=True)
    inventory = []
    destinations = set()

    def save(relative, content):
        if relative in destinations:
            raise ValueError(f"Duplicate license destination: {relative}")
        destinations.add(relative)
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        inventory.append({"path": relative.as_posix(), "sha256": hashlib.sha256(content).hexdigest()})

    def copy(source, relative):
        content = source.read_bytes()
        if source.suffix == ".gz":
            content = gzip.decompress(content)
            relative = relative.with_suffix("")
        save(relative, content)

    copy(ROOT / "LICENSE", Path("repository/LICENSE"))
    for name in VENDORS:
        directory = ROOT / "lib" / name
        if not directory.is_dir():
            raise FileNotFoundError(f"Missing pinned {name}; run scripts/fetch_dependencies.sh first")
        files = list(license_files(directory))
        if not files:
            raise ValueError(f"No license files found for {name}")
        for source in files:
            copy(source, Path("repository") / source.relative_to(ROOT))
    # These components distribute their license notice within source files.
    for relative in ("lib/lz4/lz4.c", "lib/filament/third_party/mikktspace/src/mikktspace.c"):
        save(Path("repository") / (relative + ".LICENSE.txt"), leading_comment(ROOT / relative))
    vorbis = ROOT / "benchmark/third_party/stb_vorbis.c"
    text = vorbis.read_text(encoding="utf-8")
    marker = text.index("This software is available under 2 licenses")
    start = text.rfind("/*", 0, marker)
    end = text.index("*/", marker) + 2
    save(Path("repository/benchmark/third_party/stb_vorbis.c.LICENSE.txt"),
         (text[start:end] + "\n").encode())
    for source in sorted((ROOT / "packaging/licenses").rglob("*")):
        if not source.is_file():
            continue
        copy(source, Path("repository") / source.relative_to(ROOT))
    # Asset notices live under a normally pruned assets directory, so collect
    # them explicitly; original Heaven assets are never read or copied here.
    for source in license_files(ROOT / "benchmark/assets"):
        copy(source, Path("repository") / source.relative_to(ROOT))

    labels = set()
    for index, argument in enumerate(dependency_dirs, 1):
        label, separator, location = argument.partition("=")
        directory = Path(location if separator else argument).resolve()
        if not separator:
            label = f"dependency-{index}-{directory.name}"
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", label) or label in labels:
            raise ValueError(f"Invalid or duplicate dependency label: {label}")
        labels.add(label)
        if not directory.is_dir():
            raise FileNotFoundError(f"Missing dependency license directory: {directory}")
        files = list(license_files(directory))
        if not files:
            raise ValueError(f"No license texts found in {directory}")
        for source in files:
            copy(source, Path("dependencies") / label / source.relative_to(directory))
    (output / "MANIFEST.json").write_text(
        json.dumps({"files": sorted(inventory, key=lambda entry: entry["path"])}, indent=2) + "\n",
        encoding="utf-8")
    return len(inventory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dependency-dir", action="append", default=[])
    args = parser.parse_args()
    try:
        print(f"Collected {collect(args.output, args.dependency_dir)} license and notice files.")
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
