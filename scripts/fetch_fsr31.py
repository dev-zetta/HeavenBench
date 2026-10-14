#!/usr/bin/env python3
"""Fetch the pinned official MIT FSR 3.1 source, with no media or binary DLLs."""
import argparse
import hashlib
import json
from pathlib import Path, PurePath
import tarfile
import urllib.request

COMMIT = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"
SHA256 = "eff7e8e371b2d461b980f8c75583133f771e940ed7aa00779dbbaf6dd2f88746"
URL = f"https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tar.gz/{COMMIT}"


def manifest_key(path: PurePath) -> str:
    return path.as_posix()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, default=Path("build/dependencies/fidelityfx-sdk-1.1.4"))
    p.add_argument("--archive", type=Path)
    a = p.parse_args()
    archive = a.archive or a.output.parent / "fidelityfx-sdk-1.1.4.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        urllib.request.urlretrieve(URL, temporary)
        temporary.replace(archive)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != SHA256:
        raise SystemExit(f"FSR SDK archive checksum mismatch: {digest}")
    marker = a.output / "heavenbench-source.json"
    if marker.exists():
        receipt = json.loads(marker.read_text())
        if receipt.get("archive_sha256") != SHA256 or receipt.get("commit") != COMMIT:
            raise SystemExit("Existing SDK source has a different provenance marker")
        expected_files = receipt.get("files_sha256", {})
        if not expected_files:
            raise SystemExit("Existing SDK source lacks the source integrity manifest; fetch into a fresh directory")
        for relative, expected in expected_files.items():
            path = a.output / relative
            if not path.is_file() or path.is_symlink():
                raise SystemExit(f"Missing or replaced SDK source: {relative}")
            with path.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != expected:
                raise SystemExit(f"Modified SDK source: {relative}")
        actual_files = {manifest_key(path.relative_to(a.output)) for path in a.output.rglob("*") if path.is_file() and path != marker}
        if actual_files != set(expected_files):
            raise SystemExit("Unexpected files in pinned SDK source directory")
        print(a.output.resolve())
        return
    if a.output.exists() and any(a.output.iterdir()):
        raise SystemExit("Refusing to overwrite a nonempty SDK source directory")
    a.output.mkdir(parents=True, exist_ok=True)
    prefix = f"FidelityFX-SDK-{COMMIT}/"
    with tarfile.open(archive) as tar:
        for member in tar:
            if member.name == prefix[:-1] and member.isdir():
                continue
            if not member.name.startswith(prefix):
                raise SystemExit("Unexpected SDK archive prefix")
            relative = member.name[len(prefix):]
            if not relative or not (relative.startswith("sdk/") or relative == "LICENSE.txt"):
                continue
            # Runtime component source and MIT shader compiler reflection source
            # are retained; shipped Windows binaries/sample assets are omitted.
            if "/binary_store/" in relative or relative.startswith("sdk/bin/"):
                continue
            member.name = relative
            tar.extract(member, a.output, filter="data")
    file_hashes = {}
    for path in sorted(a.output.rglob("*")):
        if path.is_file():
            with path.open("rb") as stream:
                file_hashes[manifest_key(path.relative_to(a.output))] = hashlib.file_digest(stream, "sha256").hexdigest()
    marker.write_text(json.dumps({"sdk_version": "1.1.4", "fsr_version": "3.1.4",
        "commit": COMMIT, "archive_sha256": SHA256, "url": URL,
        "files_sha256": file_hashes}, indent=2) + "\n")
    print(a.output.resolve())


if __name__ == "__main__":
    main()
