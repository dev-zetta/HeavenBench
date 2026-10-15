#!/usr/bin/env python3
"""Verify every source tree used by the pinned compute/reflection build."""
import argparse
import hashlib
from pathlib import Path, PurePath


def manifest_key(path: PurePath) -> str:
    # The reviewed lock uses POSIX keys on every host, including native Windows.
    return path.as_posix()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_source(sdk: Path, lock: Path) -> int:
    expected = {}
    for line in lock.read_text().splitlines():
        digest, relative = line.split("  ", 1)
        expected[relative] = digest
        path = sdk / relative
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"Pinned FSR source missing/replaced: {relative}")
        actual = sha256_file(path)
        if actual != digest:
            raise SystemExit(f"Pinned FSR source modified: {relative}")
    # Catch newly injected headers/shaders in any locked source directory as well.
    directories = {manifest_key(Path(relative).parent) for relative in expected}
    for relative in directories:
        # These two directories intentionally pin only one file; their other
        # upstream contents are not include paths or shader inputs of this build.
        if relative in {".", "sdk/src/backends/vk"}:
            continue
        for path in (sdk / relative).iterdir():
            if path.is_file() and manifest_key(path.relative_to(sdk)) not in expected:
                raise SystemExit(f"Unexpected source in pinned FSR build tree: {path}")
    return len(expected)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--lock", type=Path, required=True)
    args = parser.parse_args()
    print(f"Verified {verify_source(args.sdk, args.lock)} pinned FSR SDK source files")


if __name__ == "__main__":
    main()
