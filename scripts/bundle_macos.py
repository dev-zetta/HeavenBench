#!/usr/bin/env python3
"""Build extraction tools and stage a relocatable Apple Silicon app on macOS.

Only system libraries/frameworks remain external. Homebrew runtime provenance,
license texts and a hash inventory travel with the app; original Heaven data do
not. Packaging requires native Apple tools and does not notarize the result.
"""

import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"


def run(command, **options):
    result = subprocess.run([str(part) for part in command], text=True,
                            capture_output=True, check=False, **options)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n"
                           f"{result.stdout}{result.stderr}")
    return result.stdout


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def project_version():
    match = re.search(r"^project\(HeavenBench VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES",
                      (ROOT / "benchmark/CMakeLists.txt").read_text(), re.M)
    if not match:
        raise ValueError("Cannot determine HeavenBench version")
    return match[1]


def cache_values(build_dir):
    values = {}
    for line in (build_dir / "CMakeCache.txt").read_text().splitlines():
        match = re.fullmatch(r"([^#/:][^:]*):[^=]+=(.*)", line)
        if match:
            values[match[1]] = match[2]
    return values


def require_native():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("A native Apple Silicon macOS host is required")
    for tool in ("otool", "install_name_tool", "lipo", "codesign", "ditto", "xcrun", "brew"):
        if not shutil.which(tool):
            raise ValueError(f"Required tool not found: {tool}")


def require_release(cache):
    expected = {"CMAKE_BUILD_TYPE": "Release", "CMAKE_OSX_ARCHITECTURES": "arm64",
                "HEAVEN_TARGET_PLATFORM": "macos", "HEAVEN_CPU_ARCH": "portable"}
    for name, value in expected.items():
        if cache.get(name) != value:
            raise ValueError(f"Expected {name}={value}; found {cache.get(name)!r}")
    deployment = cache.get("CMAKE_OSX_DEPLOYMENT_TARGET", "")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", deployment):
        raise ValueError("Missing macOS deployment target")
    if tuple(map(int, deployment.split("."))) < (14, 0):
        raise ValueError("The Apple Silicon package baseline is macOS 14.0")


def build_tools(build_dir, jobs):
    require_native()
    cache = cache_values(build_dir)
    require_release(cache)
    # Reuse the exact configured compiler/stdlib/SDK rather than mixing the
    # system Apple C++ headers with the renderer's LLVM 21 runtime.
    names = ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS",
             "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS",
             "CMAKE_OSX_SYSROOT", "CMAKE_OSX_DEPLOYMENT_TARGET",
             "CMAKE_OSX_ARCHITECTURES", "CMAKE_PREFIX_PATH")
    args = [f"-D{name}={cache[name]}" for name in names if name in cache]
    run(["cmake", "-S", ROOT, "-B", build_dir / "tools", "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", *args])
    run(["cmake", "--build", build_dir / "tools", "--parallel", jobs])


def dependencies(output):
    result = []
    for line in output.splitlines()[1:]:
        match = re.match(r"\s+(.+?) \(compatibility version ", line)
        if match:
            result.append(match[1])
    return result


def rpaths(output):
    result = []
    lines = output.splitlines()
    for index, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for following in lines[index + 1:index + 4]:
                match = re.match(r"\s*path (.+) \(offset [0-9]+\)", following)
                if match:
                    result.append(match[1])
                    break
    return result


def minimum_macos(output):
    versions = []
    command = None
    for line in output.splitlines():
        text = line.strip()
        if text.startswith("cmd "):
            command = text[4:]
        match = re.fullmatch(r"(minos|version) ([0-9]+(?:\.[0-9]+){1,2})", text)
        if match and ((command == "LC_BUILD_VERSION" and match[1] == "minos") or
                      (command == "LC_VERSION_MIN_MACOSX" and match[1] == "version")):
            versions.append(match[2])
    if not versions:
        raise ValueError("Mach-O has no macOS minimum-version load command")
    return max(versions, key=version_tuple)


def version_tuple(value):
    parts = tuple(map(int, value.split(".")))
    return parts + (0,) * (3 - len(parts))


def is_system(name):
    return name.startswith(("/System/Library/", "/usr/lib/"))


def resolve_import(name, owner, executable_dir, search_dirs, owner_rpaths):
    def expand(path):
        if path.startswith("@loader_path/"):
            return owner.parent / path[len("@loader_path/"):]
        if path.startswith("@executable_path/"):
            return executable_dir / path[len("@executable_path/"):]
        return Path(path)

    if name.startswith("@rpath/"):
        tail = name[len("@rpath/"):]
        candidates = [expand(path) / tail for path in owner_rpaths]
        candidates.extend(directory / tail for directory in search_dirs)
    else:
        candidates = [expand(name)]
    for candidate in candidates:
        if candidate.is_absolute() and candidate.is_file():
            return candidate.resolve(strict=True)
    raise ValueError(f"Unresolved Mach-O import {name!r} in {owner}")


class Bundler:
    def __init__(self, app, brew_cellar, search_dirs, runtime_overrides):
        self.app = app
        self.frameworks = app / "Contents/Frameworks"
        self.cellar = brew_cellar.resolve(strict=True)
        self.search_dirs = search_dirs
        self.overrides = runtime_overrides
        self.by_source = {}
        self.by_name = {}
        self.records = []
        self.formulas = {}
        self.edges = []
        self.minimum_versions = {}

    def provenance(self, source):
        try:
            formula, version, *_ = source.relative_to(self.cellar).parts
            keg = self.cellar / formula / version
            if not (keg / "INSTALL_RECEIPT.json").is_file():
                raise ValueError(f"Missing Homebrew receipt: {keg}")
            self.formulas[(formula, version)] = keg
            return {"provider": "Homebrew", "formula": formula, "version": version}
        except ValueError as error:
            if source.is_relative_to(ROOT):
                return {"provider": "repository", "path": source.relative_to(ROOT).as_posix()}
            raise ValueError(f"No license/provenance source for bundled library {source}") from error

    def add_library(self, source, name=None):
        source = source.resolve(strict=True)
        name = name or source.name
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
            raise ValueError(f"Unsupported dylib filename: {name}")
        existing = self.by_name.get(name)
        if existing and existing != source:
            raise ValueError(f"Conflicting libraries named {name}: {existing}, {source}")
        if source in self.by_source:
            return self.by_source[source]
        destination = self.frameworks / name
        shutil.copy2(source, destination)
        destination.chmod(0o755)
        self.by_source[source] = destination
        self.by_name[name] = source
        self.records.append({"path": destination.relative_to(self.app).as_posix(),
                             "original_sha256": sha256(source), **self.provenance(source)})
        return destination

    def close(self, roots, seeds):
        pending = deque((source, destination, source.parent) for source, destination in roots)
        for source, name in seeds:
            pending.append((source.resolve(), self.add_library(source, name), source.parent))
        visited = set()
        while pending:
            source, destination, executable_dir = pending.popleft()
            if destination in visited:
                continue
            visited.add(destination)
            architectures = run(["lipo", "-archs", destination]).split()
            if architectures != ["arm64"]:
                if destination.parent != self.frameworks or "arm64" not in architectures:
                    raise ValueError(f"Expected native arm64 Mach-O: {destination}: {architectures}")
                thin = destination.with_name(destination.name + ".thin")
                run(["lipo", destination, "-thin", "arm64", "-output", thin])
                os.replace(thin, destination)
            load_commands = run(["otool", "-arch", "arm64", "-l", source])
            original_rpaths = rpaths(load_commands)
            self.minimum_versions[destination.relative_to(self.app).as_posix()] = minimum_macos(load_commands)
            own_ids = (run(["otool", "-arch", "arm64", "-D", source]).splitlines()[1:]
                       if destination.parent == self.frameworks else [])
            own_id = own_ids[0].strip() if own_ids else None
            changes = []
            for imported in dependencies(run(["otool", "-arch", "arm64", "-L", source])):
                if imported == own_id:
                    continue
                override = self.overrides.get(imported)
                if is_system(imported) and override is None:
                    continue
                target = override or resolve_import(imported, source, executable_dir,
                                                    self.search_dirs, original_rpaths)
                bundled = self.add_library(target)
                changes += ["-change", imported, "@rpath/" + bundled.name]
                self.edges.append({"from": destination.relative_to(self.app).as_posix(),
                                   "import": imported, "to": bundled.relative_to(self.app).as_posix()})
                pending.append((target, bundled, executable_dir))
            # Remove all build/Cellar rpaths, including transitive search paths.
            for path in dict.fromkeys(original_rpaths):
                changes += ["-delete_rpath", path]
            changes += ["-add_rpath", "@loader_path" if destination.parent == self.frameworks
                        else "@executable_path/../Frameworks"]
            if destination.parent == self.frameworks:
                changes += ["-id", "@rpath/" + destination.name]
            run(["install_name_tool", *changes, destination])
        for destination in visited:
            for imported in dependencies(run(["otool", "-arch", "arm64", "-L", destination])):
                if is_system(imported):
                    if imported in self.overrides:
                        raise ValueError(f"Unrelocated C++ runtime import: {destination}: {imported}")
                    continue
                if not imported.startswith("@rpath/") or not (self.frameworks / imported[7:]).is_file():
                    raise ValueError(f"Nonportable import after relocation: {destination}: {imported}")
            run(["codesign", "--force", "--sign", "-", "--timestamp=none", destination])
        return visited


LAUNCH_SCRIPT = r'''#!/bin/sh
set -eu
macos=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
contents=$(CDPATH= cd -- "$macos/.." && pwd)
export DYLD_LIBRARY_PATH="$contents/Frameworks"
export SDL_VULKAN_LIBRARY="$contents/Frameworks/libvulkan.1.dylib"
export VK_DRIVER_FILES="$contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export HEAVEN_RESOURCE_DIR="$contents/Resources/resources"
if [ -z "${HEAVEN_USER_DIR:-}" ]; then
    HEAVEN_USER_DIR="$HOME/Library/Application Support/HeavenBench"
fi
export HEAVEN_USER_DIR
mkdir -p "$HEAVEN_USER_DIR"
has_assets() {
    case "$1" in *.zip) [ -f "$1" ] && return 0 ;; esac
    [ -f "$1/heaven/heaven.world" ] && return 0
    for archive in "$1"/*.zip; do
        [ -f "$archive" ] && return 0
    done
    return 1
}
has_data=false
for argument in "$@"; do
    case "$argument" in --data) has_data=true ;; esac
done
if [ "$has_data" = false ] && [ -n "${HEAVEN_DATA:-}" ]; then
    set -- --data "$HEAVEN_DATA" "$@"
elif [ -z "${HEAVEN_DEFAULT_DATA:-}" ]; then
    # Asset defaults stay below a saved selection and explicit --data.
    HEAVEN_DEFAULT_DATA="$HEAVEN_USER_DIR/assets.zip"
    app_parent=$(CDPATH= cd -- "$contents/../.." && pwd)
    for candidate in "$app_parent/assets" "$app_parent/assets.zip" "$HEAVEN_USER_DIR/assets"; do
        if has_assets "$candidate"; then
            HEAVEN_DEFAULT_DATA=$candidate
            break
        fi
    done
    export HEAVEN_DEFAULT_DATA
fi
exec "$macos/heaven_benchmark" "$@"
'''

# A native CFBundleExecutable permits ordinary app code-signing. The small
# wrapper invokes the retained script by absolute path and preserves every arg.
LAUNCH_SOURCE = r'''
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *path = malloc((size_t)size + 64);
    char **args = calloc((size_t)argc + 2, sizeof(char *));
    if (!path || !args || _NSGetExecutablePath(path, &size)) return 1;
    char *slash = strrchr(path, '/');
    if (!slash) return 1;
    strcpy(slash + 1, "launch-heavenbench.sh");
    args[0] = "/bin/sh"; args[1] = path;
    for (int i = 1; i < argc; ++i) args[i + 1] = argv[i];
    execv(args[0], args);
    perror("HeavenBench launcher");
    return 1;
}
'''


def package(build_dir, output_dir):
    require_native()
    cache = cache_values(build_dir)
    require_release(cache)
    version = project_version()
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".macos-stage-", dir=output_dir) as temporary:
        stage = Path(temporary)
        app = stage / "HeavenBench.app"
        macos = app / "Contents/MacOS"
        resources = app / "Contents/Resources"
        frameworks = app / "Contents/Frameworks"
        for directory in (macos, resources, frameworks):
            directory.mkdir(parents=True)
        roots = []
        for relative in ("heaven_benchmark", "heaven_asset_packer", "tools/uniginex", "tools/mesh2obj"):
            source = build_dir / relative
            if not source.is_file() or not os.access(source, os.X_OK):
                raise ValueError(f"Missing executable: {source}")
            destination = macos / source.name
            shutil.copy2(source, destination)
            destination.chmod(0o755)
            roots.append((source, destination))
        launcher_source = stage / "launcher.c"
        launcher_source.write_text(LAUNCH_SOURCE)
        launcher = macos / "HeavenBench"
        run([cache["CMAKE_C_COMPILER"], "-arch", "arm64", "-isysroot", cache["CMAKE_OSX_SYSROOT"],
             "-mmacosx-version-min=" + cache["CMAKE_OSX_DEPLOYMENT_TARGET"],
             "-Wl,-headerpad_max_install_names", "-O2", launcher_source, "-o", launcher])
        roots.append((launcher, launcher))
        (macos / "launch-heavenbench.sh").write_text(LAUNCH_SCRIPT)
        (macos / "launch-heavenbench.sh").chmod(0o755)
        shutil.copytree(ROOT / "benchmark/assets", resources / "resources")
        shutil.copy2(ROOT / "tools/extraction/assemble_scene.py", resources)
        (resources / "docs").mkdir()
        shutil.copy2(ROOT / "docs/ZIP_ASSET_STORAGE.md", resources / "docs")
        # The external asset boundary is explicit; no original input is read.
        (resources / "ASSETS.txt").write_text(
            "Original Unigine Heaven archives, scenes and generated texture packs are not included.\n"
            "Obtain Heaven from https://benchmark.unigine.com/heaven and use only assets you may use.\n"
            "See docs/ZIP_ASSET_STORAGE.md. Set HEAVEN_DATA, use --data, or import an asset pack.\n"
            "Default user data: ~/Library/Application Support/HeavenBench/assets.zip\n")
        (resources / "README.txt").write_text(
            "HeavenBench for Apple Silicon (M1 or newer), macOS " + cache["CMAKE_OSX_DEPLOYMENT_TARGET"] + "+\n\n"
            "Extract the ZIP and open HeavenBench.app. Original Heaven assets are supplied separately; see ASSETS.txt.\n"
            "Configuration, cache and reports use ~/Library/Application Support/HeavenBench.\n"
            "HEAVEN_USER_DIR overrides writable storage; HEAVEN_DATA or --data chooses external assets.\n"
            "CLI: HeavenBench.app/Contents/MacOS/HeavenBench --help\n"
            "Standalone packer/extraction tools are in Contents/MacOS; assemble_scene.py requires Python 3.\n"
            "Vulkan runs through the bundled MoltenVK translation layer. Output is SDR; HDR10 requires native Wayland.\n"
            "This app is ad-hoc signed, without Developer ID or Apple notarization. Gatekeeper may require explicit approval.\n"
            "Hosted CI/CLI verification does not qualify physical GPU/display behavior.\n"
            "licenses/ contains redistributed notices and runtime provenance. package-manifest.json hashes shipped files.\n")
        plist = {"CFBundleName": "HeavenBench", "CFBundleDisplayName": "HeavenBench",
                 "CFBundleIdentifier": "app.zetta.HeavenBench", "CFBundleExecutable": "HeavenBench",
                 "CFBundlePackageType": "APPL", "CFBundleVersion": version,
                 "CFBundleShortVersionString": version, "LSMinimumSystemVersion": cache["CMAKE_OSX_DEPLOYMENT_TARGET"],
                 "NSHighResolutionCapable": True, "NSPrincipalClass": "NSApplication"}
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(plist))
        cellar = Path(run(["brew", "--cellar"]).strip())
        prefixes = {name: Path(run(["brew", "--prefix", name]).strip()).resolve(strict=True)
                    for name in ("sdl2", "glm", "vulkan-loader", "molten-vk")}
        llvm = Path(cache["CMAKE_CXX_COMPILER"]).resolve(strict=True).parent.parent
        if not llvm.is_relative_to(cellar):
            raise ValueError("Release packaging requires a Homebrew LLVM keg with redistributed notices")
        prefixes["llvm"] = llvm
        loader = prefixes["vulkan-loader"] / "lib/libvulkan.1.dylib"
        icds = list(prefixes["molten-vk"].rglob("MoltenVK_icd.json"))
        if len(icds) != 1:
            raise ValueError(f"Expected one installed MoltenVK ICD, found {icds}")
        icd = json.loads(icds[0].read_text())
        molten = (icds[0].parent / icd["ICD"]["library_path"]).resolve(strict=True)
        cxx = (llvm / "lib/c++/libc++.1.dylib").resolve(strict=True)
        unwind = (llvm / "lib/unwind/libunwind.1.dylib").resolve(strict=True)
        overrides = {"/usr/lib/libc++.1.dylib": cxx, "/usr/lib/libunwind.1.dylib": unwind}
        abi = llvm / "lib/c++/libc++abi.1.dylib"
        if abi.is_file():
            overrides["/usr/lib/libc++abi.dylib"] = abi.resolve()
            overrides["/usr/lib/libc++abi.1.dylib"] = abi.resolve()
        search = [llvm / "lib/c++", llvm / "lib/unwind"]
        search += [prefix / "lib" for prefix in prefixes.values()]
        bundler = Bundler(app, cellar, search, overrides)
        binaries = bundler.close(roots, [(loader, "libvulkan.1.dylib"), (molten, molten.name),
                                        (cxx, cxx.name), (unwind, unwind.name)])
        actual_minimum = max([cache["CMAKE_OSX_DEPLOYMENT_TARGET"], *bundler.minimum_versions.values()],
                             key=version_tuple)
        plist["LSMinimumSystemVersion"] = actual_minimum
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(plist))
        readme = resources / "README.txt"
        readme.write_text(readme.read_text().replace(
            "macOS " + cache["CMAKE_OSX_DEPLOYMENT_TARGET"] + "+",
            "macOS " + actual_minimum + "+"))
        # The ICD lives in Resources/vulkan/icd.d, three levels from Contents.
        icd["ICD"]["library_path"] = "../../../Frameworks/" + bundler.by_source[molten].name
        icd_dir = resources / "vulkan/icd.d"
        icd_dir.mkdir(parents=True)
        (icd_dir / "MoltenVK_icd.json").write_text(json.dumps(icd, indent=2) + "\n")
        notices = [f"{formula.replace('@', '-')}-{version}={keg}"
                   for (formula, version), keg in sorted(bundler.formulas.items())]
        # GLM is header-only; it will not appear in the dynamic dependency graph.
        notices.append("GLM=" + str(prefixes["glm"]))
        run([sys.executable, SCRIPTS / "collect_licenses.py", "--output", resources / "licenses",
             *[argument for notice in notices for argument in ("--dependency-dir", notice)]])
        for record in bundler.records:
            record["sha256"] = sha256(app / record["path"])
        inventory = {"platform": "macos-arm64", "graphics": "Vulkan via MoltenVK",
                     "system_imports_external": ["/System/Library/", "/usr/lib/"],
                     "libraries": sorted(bundler.records, key=lambda record: record["path"]),
                     "imports": bundler.edges, "static_dependencies": ["GLM"],
                     "minimum_macos": actual_minimum, "binary_minimum_macos": bundler.minimum_versions,
                     "signing": "ad-hoc; no Developer ID or notarization"}
        (resources / "licenses/macos-runtime.json").write_text(json.dumps(inventory, indent=2) + "\n")
        # Outer signing changes the native CFBundleExecutable's signature to
        # seal the final resource manifest. Its bytes are covered by codesign
        # and the ZIP SHA, avoiding a self-referential hash manifest.
        files = [{"path": path.relative_to(app).as_posix(), "sha256": sha256(path)}
                 for path in sorted(app.rglob("*")) if path.is_file() and path != launcher]
        (resources / "package-manifest.json").write_text(json.dumps(
            {"version": version, "platform": "macos-arm64", "cpu": "arm64",
             "minimum_macos": actual_minimum, "files": files,
             "manifest_excludes": ["Contents/Resources/package-manifest.json", "Contents/_CodeSignature/**",
                                   "Contents/MacOS/HeavenBench"],
             "excluded_launcher_integrity": "sealed by app codesign and archive SHA-256"}, indent=2) + "\n")
        run(["codesign", "--force", "--sign", "-", "--timestamp=none", app])
        run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", app])
        for binary in binaries:
            run(["codesign", "--verify", "--strict", binary])
        archive = output_dir / f"HeavenBench-{version}-macos-arm64.zip"
        temporary_archive = stage / "package.zip"
        run(["ditto", "-c", "-k", "--keepParent", "--norsrc", app, temporary_archive])
        with zipfile.ZipFile(temporary_archive) as check:
            if check.testzip() is not None:
                raise ValueError("ZIP CRC verification failed")
        os.replace(temporary_archive, archive)
        sidecar = archive.with_suffix(archive.suffix + ".sha256")
        temporary_sidecar = stage / "package.sha256"
        temporary_sidecar.write_text(f"{sha256(archive)}  {archive.name}\n")
        os.replace(temporary_sidecar, sidecar)
        print(f"Packaged {archive}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("version")
    tools = commands.add_parser("tools")
    tools.add_argument("--build-dir", type=Path, required=True)
    tools.add_argument("--jobs", type=int, default=2)
    bundle = commands.add_parser("package")
    bundle.add_argument("--build-dir", type=Path, required=True)
    bundle.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "version":
            print(project_version())
        elif args.command == "tools":
            if args.jobs < 1:
                raise ValueError("--jobs must be positive")
            build_tools(args.build_dir.resolve(), args.jobs)
        else:
            package(args.build_dir.resolve(), args.output_dir.resolve())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
