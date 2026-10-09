#!/usr/bin/env python3
"""Check a relocated AppImage without FUSE, display, or original Heaven data."""

import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run_checked(command, **options):
    result = subprocess.run(command, check=False, text=True, capture_output=True, **options)
    if result.returncode != 0:
        raise SystemExit(f"{command[0]} exited {result.returncode}:\n{result.stderr or result.stdout}")
    return result


def main() -> None:
    artifact = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="heavenbench package ") as temporary:
        root = Path(temporary)
        relocated = root / artifact.name
        shutil.copy2(artifact, relocated)
        subprocess.run([str(relocated), "--appimage-extract"], cwd=root,
                       stdout=subprocess.DEVNULL, check=True, timeout=120)
        appdir = root / "squashfs-root"
        env = dict(os.environ, APPIMAGE=str(relocated), APPDIR=str(appdir),
                   HEAVEN_USER_DIR=str(root / "user files"))
        for name in ("LD_LIBRARY_PATH", "HEAVEN_DATA", "HEAVEN_DEFAULT_DATA", "HEAVEN_RESOURCE_DIR"):
            env.pop(name, None)
        launcher = str(appdir / "AppRun")
        for forbidden in ("libvulkan*", "libGL*", "libEGL*", "libOpenGL*", "libgbm*", "libdrm*", "libwayland-client*"):
            if list((appdir / "usr/lib").rglob(forbidden)):
                raise SystemExit(f"Host graphics library bundled: {forbidden}")
        if list(appdir.rglob("*.ung")) or list(appdir.rglob("assets.zip")):
            raise SystemExit("Original Heaven assets must stay outside the package")
        help_result = run_checked([launcher, "--help"], cwd=root, env=env, timeout=20)
        if "--data" not in help_result.stdout:
            raise SystemExit("Packaged CLI help is incomplete")
        packer = str(appdir / "usr/bin/heaven_asset_packer")
        packer_help = run_checked([packer, "--help"], cwd=root, env=env, timeout=20)
        if "heaven_asset_packer" not in packer_help.stdout:
            raise SystemExit("Standalone asset packer help is incomplete")
        packer_target = root / "must-not-exist.zip"
        packer_missing = subprocess.run(
            [packer, "--data", "absent assets.zip", "--pack-assets", str(packer_target)],
            cwd=root, env=env, text=True, capture_output=True, timeout=20, check=False)
        if packer_missing.returncode != 2 or packer_target.exists():
            raise SystemExit(f"Standalone packer missing-assets failure: {packer_missing.stderr}")
        config = root / "explicit config.json"
        run_checked([launcher, "--data", "relative assets.zip", "--write-config", config.name],
                    cwd=root, env=env, timeout=20)
        effective = json.loads(config.read_text())
        if effective["data_dir"] != "relative assets.zip":
            raise SystemExit("Launcher changed an explicit relative asset path")
        if Path(effective["tessellation.cache_file"]).parent != root / "user files/cache":
            raise SystemExit("Packaged cache is outside writable user storage")
        # Saved asset selections must survive package-default discovery.
        (root / "user files/config.json").write_text('{"data_dir":"saved assets.zip"}')
        run_checked([launcher, "--write-config", config.name], cwd=root, env=env, timeout=20)
        if json.loads(config.read_text())["data_dir"] != "saved assets.zip":
            raise SystemExit("Launcher default overrides the saved asset selection")
        # The shipped binary must fail cleanly before GPU setup when no assets
        # were supplied; no renderer build checkout should be needed to run it.
        missing = subprocess.run([launcher, "--data", "absent assets.zip"],
                                 cwd=root, env=env, text=True, capture_output=True, timeout=20)
        if missing.returncode != 1:
            raise SystemExit(f"Missing-assets launch returned {missing.returncode}: {missing.stderr}")
        resources = appdir / "usr/share/heavenbench/resources"
        for relative in ("sky/moon_disk.png", "sky/moon_normal.png", "sky/milkyway.png",
                         "loading/loading_screen.png", "attribution/DeepBump.md", "attribution/bcdec.md"):
            if not (resources / relative).is_file():
                raise SystemExit(f"Packaged renderer resource missing: {relative}")
        licenses = appdir / "usr/share/licenses/HeavenBench"
        for directory in (licenses, licenses / "appimage-runtime"):
            manifest = json.loads((directory / "MANIFEST.json").read_text())
            for entry in manifest["files"]:
                path = directory / entry["path"]
                if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
                    raise SystemExit(f"Packaged notice checksum mismatch: {path}")
        system = json.loads((licenses / "system/MANIFEST.json").read_text())
        for name in ("static:GLM", "static:LLVM libc++", "static:LLVM libc++abi"):
            if not system.get(name):
                raise SystemExit(f"Static dependency notice missing: {name}")
        for notices in system.values():
            for relative in notices:
                if not (appdir / relative).is_file():
                    raise SystemExit(f"System dependency notice missing: {relative}")
        print("PASS: relocated AppImage/packer help, config write, missing-assets failures, resources and notices")


if __name__ == "__main__":
    main()
