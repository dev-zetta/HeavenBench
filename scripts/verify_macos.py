#!/usr/bin/env python3
"""Verify a relocated macOS ZIP and CLI without claiming GPU/notarization acceptance."""

import argparse
import ctypes
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import stat
import subprocess
import tempfile
import time
import zipfile

from bundle_macos import dependencies, is_system, minimum_macos, rpaths, run, sha256, version_tuple


def checked(command, root, env, expected=0):
    result = subprocess.run([str(value) for value in command], cwd=root, env=env,
                            text=True, capture_output=True, timeout=30, check=False)
    if result.returncode != expected:
        raise ValueError(f"{command} returned {result.returncode}, expected {expected}:\n"
                         f"{result.stdout}{result.stderr}")
    return result.stdout


def safe_members(archive):
    names = set()
    for member in archive.infolist():
        path = PurePosixPath(member.filename)
        if (path.is_absolute() or ".." in path.parts or "\\" in member.filename or
                not path.parts or path.parts[0] != "HeavenBench.app" or member.filename in names):
            raise ValueError(f"Unsafe or duplicate ZIP path: {member.filename}")
        names.add(member.filename)
        mode = member.external_attr >> 16
        if stat.S_ISLNK(mode):
            raise ValueError(f"Unexpected ZIP symlink: {member.filename}")
    if not names or archive.testzip() is not None:
        raise ValueError("Empty ZIP or failed CRC")
    return names


def resolve_icd_library(icd_path, frameworks):
    icd = json.loads(icd_path.read_text())
    molten = (icd_path.parent / icd["ICD"]["library_path"]).resolve(strict=True)
    if not molten.is_relative_to(frameworks.resolve(strict=True)):
        raise ValueError("ICD library escapes bundled Frameworks")
    return molten


def verify(artifact):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("Relocated verification requires native Apple Silicon macOS")
    declared = artifact.with_suffix(artifact.suffix + ".sha256").read_text().strip().split()
    if declared != [sha256(artifact), artifact.name]:
        raise ValueError("ZIP SHA-256 sidecar mismatch")
    with zipfile.ZipFile(artifact) as archive:
        members = safe_members(archive)
    with tempfile.TemporaryDirectory(prefix="heavenbench relocated macos ") as temporary:
        root = Path(temporary).resolve()
        unpacked = root / "unpacked app with spaces"
        run(["ditto", "-x", "-k", artifact, unpacked])
        app = unpacked / "HeavenBench.app"
        contents = app / "Contents"
        resources = contents / "Resources"
        macos = contents / "MacOS"
        frameworks = contents / "Frameworks"
        run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", app])
        plist = plistlib.loads((contents / "Info.plist").read_bytes())
        manifest = json.loads((resources / "package-manifest.json").read_text())
        inventory = json.loads((resources / "licenses/macos-runtime.json").read_text())
        files = {entry["path"]: entry["sha256"] for entry in manifest["files"]}
        if len(files) != len(manifest["files"]):
            raise ValueError("Duplicate package manifest entries")
        for relative, digest in files.items():
            if not (app / relative).is_relative_to(app) or ".." in PurePosixPath(relative).parts:
                raise ValueError("Unsafe package manifest path")
            if sha256(app / relative) != digest:
                raise ValueError(f"Package manifest checksum mismatch: {relative}")
        excluded = {"Contents/Resources/package-manifest.json", "Contents/MacOS/HeavenBench"}
        actual_files = {path.relative_to(app).as_posix() for path in app.rglob("*") if path.is_file()
                        and not path.is_relative_to(contents / "_CodeSignature")}
        if actual_files != set(files) | excluded:
            raise ValueError("Package manifest file coverage mismatch")
        for path in app.rglob("*"):
            if path.suffix.lower() == ".ung" or path.name.lower() in ("assets.zip", "heaven.world"):
                raise ValueError(f"Original Heaven content bundled: {path}")
        notice_manifest = json.loads((resources / "licenses/MANIFEST.json").read_text())
        for entry in notice_manifest["files"]:
            if sha256(resources / "licenses" / entry["path"]) != entry["sha256"]:
                raise ValueError(f"Notice checksum mismatch: {entry['path']}")
        required = ("sky/moon_disk.png", "sky/moon_normal.png", "sky/milkyway.png",
                    "loading/loading_screen.png", "attribution/DeepBump.md", "attribution/bcdec.md")
        for relative in required:
            if not (resources / "resources" / relative).is_file():
                raise ValueError(f"Missing renderer resource: {relative}")
        binaries = [macos / name for name in ("HeavenBench", "heaven_benchmark", "heaven_asset_packer", "uniginex", "mesh2obj")]
        binaries += [path for path in frameworks.iterdir() if path.is_file()]
        minimums = {}
        for binary in binaries:
            if run(["lipo", "-archs", binary]).strip() != "arm64":
                raise ValueError(f"Non-arm64 payload: {binary}")
            commands = run(["otool", "-l", binary])
            minimums[binary.relative_to(app).as_posix()] = minimum_macos(commands)
            expected_path = "@loader_path" if binary.parent == frameworks else "@executable_path/../Frameworks"
            if rpaths(commands) != [expected_path]:
                raise ValueError(f"Nonportable LC_RPATH: {binary}: {rpaths(commands)}")
            for imported in dependencies(run(["otool", "-L", binary])):
                if is_system(imported):
                    if imported in ("/usr/lib/libc++.1.dylib", "/usr/lib/libc++abi.dylib", "/usr/lib/libc++abi.1.dylib", "/usr/lib/libunwind.1.dylib"):
                        raise ValueError(f"Mixed system C++/unwind runtime: {binary}: {imported}")
                elif not imported.startswith("@rpath/") or not (frameworks / imported[7:]).is_file():
                    raise ValueError(f"Unresolved relocated import: {binary}: {imported}")
            run(["codesign", "--verify", "--strict", binary])
        actual_minimum = max(minimums.values(), key=version_tuple)
        if (minimums != inventory["binary_minimum_macos"] or
                any(value != actual_minimum for value in (inventory["minimum_macos"], manifest["minimum_macos"], plist["LSMinimumSystemVersion"]))):
            raise ValueError("Actual Mach-O baseline and declared minimum macOS differ")
        for entry in inventory["libraries"]:
            if sha256(app / entry["path"]) != entry["sha256"]:
                raise ValueError(f"Runtime inventory mismatch: {entry['path']}")
        icd_path = resources / "vulkan/icd.d/MoltenVK_icd.json"
        molten = resolve_icd_library(icd_path, frameworks)
        # A dlopen/symbol check establishes library closure without enumerating
        # GPUs or creating a graphics instance/device/window.
        for library in (frameworks / "libvulkan.1.dylib", molten):
            handle = ctypes.CDLL(str(library))
            getattr(handle, "vkGetInstanceProcAddr")
        env = dict(os.environ)
        for name in list(env):
            if name.startswith(("DYLD_", "VK_")) or name in ("HEAVEN_DATA", "HEAVEN_RESOURCE_DIR", "HEAVEN_DEFAULT_DATA", "SDL_VULKAN_LIBRARY"):
                env.pop(name)
        env.update(PATH="/usr/bin:/bin:/usr/sbin:/sbin", HEAVEN_USER_DIR=str(root / "writable user data"))
        launcher = macos / "HeavenBench"
        help_text = checked([launcher, "--help"], root, env)
        if "--data" not in help_text:
            raise ValueError("Incomplete launcher CLI help")
        checked([macos / "heaven_benchmark", "--help"], root, env)
        packer = macos / "heaven_asset_packer"
        if "heaven_asset_packer" not in checked([packer, "--help"], root, env):
            raise ValueError("Incomplete standalone packer help")
        target = root / "must-not-exist.zip"
        checked([packer, "--data", "absent assets.zip", "--pack-assets", target], root, env, expected=2)
        if target.exists():
            raise ValueError("Failed packer wrote an archive")
        config = root / "effective config.json"
        checked([launcher, "--write-config", config], root, env)
        effective = json.loads(config.read_text())
        user = Path(env["HEAVEN_USER_DIR"])
        if effective["data_dir"] != str(user / "assets.zip"):
            raise ValueError("Launcher default is not user asset storage")
        if Path(effective["tessellation.cache_file"]).parent != user / "cache":
            raise ValueError("Cache escaped writable user directory")
        # Adjacent assets remain external to the signed, immutable app.
        (unpacked / "assets.zip").write_bytes(b"default discovery fixture")
        checked([launcher, "--write-config", config], root, env)
        if json.loads(config.read_text())["data_dir"] != str(unpacked / "assets.zip"):
            raise ValueError("Launcher missed its adjacent external asset pack")
        (user / "config.json").write_text('{"data_dir":"saved assets.zip"}')
        checked([launcher, "--write-config", config], root, env)
        if json.loads(config.read_text())["data_dir"] != "saved assets.zip":
            raise ValueError("Launcher default overwrote saved assets")
        explicit = dict(env, HEAVEN_DATA="environment-assets.zip")
        checked([launcher, "--write-config", config], root, explicit)
        if json.loads(config.read_text())["data_dir"] != "environment-assets.zip":
            raise ValueError("HEAVEN_DATA did not override saved assets")
        checked([launcher, "--data", "relative assets.zip", "--write-config", config], root, explicit)
        if json.loads(config.read_text())["data_dir"] != "relative assets.zip":
            raise ValueError("Explicit relative asset path was changed")
        checked([launcher, "--data", "absent assets.zip"], root, env, expected=1)
        return {"status": "passed", "scope": "native arm64 package integrity, library closure and relocated CLI",
                "artifact": artifact.name, "sha256": sha256(artifact), "zip_members": len(members),
                "payload_hashes": len(files), "notice_hashes": len(notice_manifest["files"]),
                "minimum_macos": actual_minimum, "binary_minimum_macos": minimums,
                "host": {"macos": platform.mac_ver()[0], "architecture": platform.machine()},
                "physical_gpu_display_acceptance": False, "developer_id_signed": False, "notarized": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.artifact.resolve(strict=True))
    except (OSError, ValueError, RuntimeError, KeyError, AttributeError) as error:
        result = {"status": "failed", "error": str(error), "physical_gpu_display_acceptance": False}
    result["checked_unix"] = time.time()
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.receipt.with_suffix(".json.part")
    temporary.write_text(json.dumps(result, indent=2) + "\n")
    os.replace(temporary, args.receipt)
    print(json.dumps(result))
    raise SystemExit(0 if result["status"] == "passed" else 1)


if __name__ == "__main__":
    main()
