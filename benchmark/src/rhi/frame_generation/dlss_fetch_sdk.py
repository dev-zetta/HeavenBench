#!/usr/bin/env python3
"""Fetch the verified official archive; extract only Streamline headers/license."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
import tempfile
import urllib.request

VERSION = "2.14.1"
URL = f"https://github.com/NVIDIA-RTX/Streamline/archive/refs/tags/v{VERSION}.tar.gz"
SHA256 = "34d6e8b70a641c734299a6430aa8ef5be7d7ba8de28a47186b5707e439bd645b"
MARKER = "heavenbench-source.json"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_existing(output):
    marker = output / MARKER
    if not marker.is_file() or marker.is_symlink():
        raise SystemExit("Existing Streamline directory has no regular provenance marker")
    receipt = json.loads(marker.read_text())
    hashes = receipt.get("files_sha256", {})
    if receipt.get("version") != VERSION or receipt.get("archive_sha256") != SHA256 or not hashes:
        raise SystemExit("Existing Streamline headers have different provenance")
    actual = set()
    for path in output.rglob("*"):
        if path.is_symlink():
            raise SystemExit("Symlink in pinned Streamline headers")
        if path.is_file() and path != marker:
            relative = path.relative_to(output).as_posix()
            actual.add(relative)
            if hashes.get(relative) != digest(path):
                raise SystemExit(f"Modified or unexpected Streamline file: {relative}")
    if actual != set(hashes):
        raise SystemExit("Pinned Streamline headers are incomplete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/dependencies/streamline-2.14.1-headers"))
    parser.add_argument("--archive", type=Path, help="Use an existing official archive without network access")
    parser.add_argument("--verify-only", action="store_true", help="Check existing headers without downloading or writing files")
    args = parser.parse_args()
    if args.verify_only:
        if not args.output.is_dir():
            raise SystemExit("Verified Streamline headers are missing; run the explicit fetch helper first")
        verify_existing(args.output)
        print(args.output.resolve())
        return
    if args.output.exists():
        verify_existing(args.output)
        print(args.output.resolve())
        return
    archive = args.archive or args.output.parent / f"streamline-{VERSION}.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        urllib.request.urlretrieve(URL, temporary)
        temporary.replace(archive)
    actual = digest(archive)
    if actual != SHA256:
        raise SystemExit(f"Streamline archive checksum mismatch: {actual}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    prefix = f"Streamline-{VERSION}/"
    with tempfile.TemporaryDirectory(prefix="streamline-headers-", dir=args.output.parent) as stage:
        payload = Path(stage) / "payload"
        payload.mkdir()
        with tarfile.open(archive) as source:
            for member in source:
                if member.name == prefix.rstrip("/") and member.isdir():
                    continue
                if not member.name.startswith(prefix):
                    raise SystemExit("Unexpected Streamline archive prefix")
                relative = member.name[len(prefix):]
                if not (relative.startswith("include/") or relative == "license.txt"):
                    continue
                name = PurePosixPath(relative)
                if name.is_absolute() or ".." in name.parts or member.issym() or member.islnk():
                    raise SystemExit("Unsafe Streamline archive member")
                if member.isdir():
                    continue
                if not member.isfile():
                    raise SystemExit("Unsupported Streamline archive member")
                destination = payload.joinpath(*name.parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(member) as stream:
                    destination.write_bytes(stream.read())
        if not (payload / "include/sl.h").is_file() or not (payload / "license.txt").is_file():
            raise SystemExit("Streamline archive lacks required headers/license")
        hashes = {path.relative_to(payload).as_posix(): digest(path)
                  for path in sorted(payload.rglob("*")) if path.is_file()}
        (payload / MARKER).write_text(json.dumps({"version": VERSION, "url": URL,
            "archive_sha256": SHA256, "files_sha256": hashes}, indent=2) + "\n")
        payload.replace(args.output)
    print(args.output.resolve())


if __name__ == "__main__":
    main()
