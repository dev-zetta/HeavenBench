#!/usr/bin/env python3
"""Archive the exact filtered Docker source context, including uncommitted edits."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    args = parser.parse_args()
    source = args.source_root.resolve(strict=True)
    output = args.output_dir.resolve()
    if output == source or source in output.parents:
        parser.error("Output must be outside the filtered source context")
    output.mkdir(parents=True, exist_ok=True)
    version = re.search(r"project\(HeavenBench VERSION ([0-9.]+)",
                        (source / "benchmark/CMakeLists.txt").read_text())[1]
    prefix = f"HeavenBench-{version}"
    files = sorted(path for path in source.rglob("*") if path.is_file() or path.is_symlink())
    manifest = {}
    for path in files:
        if path.is_symlink():
            # No link may escape the archived source tree.
            if path.readlink().is_absolute() or not path.resolve().is_relative_to(source):
                raise SystemExit(f"Source symlink escapes context: {path.relative_to(source)}")
            manifest[path.relative_to(source).as_posix()] = {"symlink": str(path.readlink())}
        else:
            manifest[path.relative_to(source).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    archive = output / f"{prefix}-source.tar.gz"
    with archive.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as tar:
            for path in files:
                info = tar.gettarinfo(str(path), arcname=f"{prefix}/{path.relative_to(source).as_posix()}")
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                if info.isfile():
                    with path.open("rb") as content:
                        tar.addfile(info, content)
                else:
                    tar.addfile(info)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(archive.suffix + ".sha256").write_text(f"{digest}  {archive.name}\n")
    (output / "source-manifest.json").write_text(json.dumps({
        "platform": args.platform, "source_archive": archive.name,
        "source_sha256": digest, "files": manifest,
        "source_policy": "Exact Docker context; includes local edits; dependencies fetched from checked-in pins.",
    }, indent=2, sort_keys=True) + "\n")
    environment = []
    for command in (["dpkg-query", "-W"], ["clang-21", "--version"], ["cmake", "--version"]):
        try:
            result = subprocess.run(command, text=True, capture_output=True, check=True)
            environment.append("$ " + " ".join(command) + "\n" + result.stdout)
        except (OSError, subprocess.CalledProcessError) as error:
            environment.append("$ " + " ".join(command) + "\nUnavailable: " + str(error))
    (output / "build-environment.txt").write_text("\n".join(environment))
    print(f"Archived exact build context: {archive.name} ({digest})")


if __name__ == "__main__":
    main()
