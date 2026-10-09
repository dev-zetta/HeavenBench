#!/usr/bin/env python3
"""Build runtime assets from the pinned Heaven installer without executing it.

The Python pipeline accepts existing native tools on Linux or Windows. Only
automatic compilation uses the repository's Linux build helpers. InstallerSpec
is injectable by offline tests; the public CLI always uses the official pin.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import uuid
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALLER_NAME = "Unigine_Heaven-4.0.run"
CHUNK_SIZE = 1024 * 1024
ARCHIVE_MEMBERS = {"./data/core.ung": "core", "./data/heaven.ung": "heaven"}
MAX_UNG_BYTES = 512 * 1024 * 1024
# Release base packs must support features enabled after installation. Keep
# this archive-path inventory aligned with engine/feature_assets.h; the test
# suite checks that contract. The general used-asset packer remains selective.
REQUIRED_FEATURE_ASSETS = {
    "heaven/heaven/meshes/environment/fly_island1.mesh",
    "heaven/heaven/meshes/environment/fly_island2.mesh",
    "heaven/heaven/meshes/environment/fly_island3.mesh",
    "heaven/heaven/textures/environment/grass_d.dds",
    "heaven/heaven/textures/environment/grass_irregularity_d.dds",
    "heaven/heaven/textures/environment/grass_variation_d.dds",
    "heaven/heaven/textures/environment/clouds1_d.dds",
    "heaven/heaven/textures/environment/clouds2_d.dds",
    "heaven/heaven/textures/environment/clouds_mask0_d.dds",
    "heaven/heaven/textures/sfx/fire_glow_dr.dds",
    "heaven/heaven/textures/sfx/spark_d.dds",
}


@dataclass(frozen=True)
class InstallerSpec:
    url: str
    size: int
    sha256: str
    payload_offset: int


OFFICIAL_INSTALLER = InstallerSpec(
    url="https://assets.unigine.com/d/Unigine_Heaven-4.0.run",
    size=286332610,
    sha256="52db2e5dedd5621d7c2bfa936b482c0a7cc69b30616278ef199513d494d8e397",
    payload_offset=15051,
)


class BuildError(RuntimeError):
    """A recoverable build failure, reported without a Python traceback."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(CHUNK_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_installer(path: Path, spec: InstallerSpec = OFFICIAL_INSTALLER) -> dict:
    if not path.is_file() or path.is_symlink():
        raise BuildError(f"Installer must be a regular file, not a symlink: {path}")
    size = path.stat().st_size
    if size != spec.size:
        raise BuildError(f"Installer size mismatch: {path} ({size}, expected {spec.size})")
    digest = sha256_file(path)
    if digest != spec.sha256:
        raise BuildError(f"Installer SHA-256 mismatch; existing file was preserved: {path}")
    return {"bytes": size, "sha256": digest}


def select_installer(explicit: Path | None, repo_root: Path, cwd: Path) -> Path:
    if explicit is not None:
        return explicit.expanduser().absolute()
    candidates = [cwd / INSTALLER_NAME, repo_root / INSTALLER_NAME,
                  repo_root / "data" / INSTALLER_NAME]
    for candidate in candidates:
        if candidate.exists() or candidate.is_symlink():
            return candidate.absolute()
    return candidates[-1].absolute()


def require_https(url: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise BuildError(f"Installer download requires an HTTPS URL without credentials: {url}")


class HttpsOnlyRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        require_https(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def publish_file(source: Path, destination: Path, *, overwrite: bool) -> None:
    """Publish on the same filesystem; never replace a raced-in file implicitly."""
    if overwrite:
        os.replace(source, destination)
    elif os.name == "nt":
        # Windows rename refuses an existing destination and also works on
        # filesystems, such as exFAT, that do not support hard links.
        os.rename(source, destination)
    else:
        # Unlike POSIX rename(), creating a hard link fails atomically if the
        # destination appeared since the initial conflict check. The staging
        # directory cleanup removes the temporary name after publication.
        os.link(source, destination)


def download_installer(destination: Path, spec: InstallerSpec = OFFICIAL_INSTALLER) -> dict:
    require_https(spec.url)
    if destination.exists() or destination.is_symlink():
        raise BuildError(f"Refusing to replace an existing installer: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=f".{destination.name}.", suffix=".part",
                                        dir=destination.parent)
    partial = Path(name)
    try:
        digest = hashlib.sha256()
        size = 0
        with os.fdopen(descriptor, "wb") as output:
            opener = urllib.request.build_opener(HttpsOnlyRedirectHandler())
            request = urllib.request.Request(spec.url, headers={"User-Agent": "HeavenBench-asset-builder/1"})
            with opener.open(request, timeout=60) as response:
                require_https(response.geturl())
                announced = response.headers.get("Content-Length")
                if announced is not None and announced != str(spec.size):
                    raise BuildError("Installer response Content-Length differs from its pinned size")
                while block := response.read(CHUNK_SIZE):
                    size += len(block)
                    if size > spec.size:
                        raise BuildError("Installer download exceeded its pinned size")
                    output.write(block)
                    digest.update(block)
            output.flush()
            os.fsync(output.fileno())
        if size != spec.size or digest.hexdigest() != spec.sha256:
            raise BuildError("Installer download failed pinned size/SHA-256 verification")
        publish_file(partial, destination, overwrite=False)
        return {"bytes": size, "sha256": digest.hexdigest()}
    except (OSError, http.client.HTTPException) as error:
        raise BuildError(f"Installer download failed: {error}") from error
    finally:
        partial.unlink(missing_ok=True)


def acquire_installer(path: Path, *, no_download: bool = False,
                      spec: InstallerSpec = OFFICIAL_INSTALLER) -> dict:
    if path.exists() or path.is_symlink():
        return {**verify_installer(path, spec), "downloaded": False}
    if no_download:
        raise BuildError(f"Installer is absent and --no-download was specified: {path}")
    print(f"==> Downloading {spec.url}", flush=True)
    return {**download_installer(path, spec), "downloaded": True}


def safe_member_name(name: str) -> str:
    if "\\" in name or "\x00" in name or name.startswith("/") or ":" in name:
        raise BuildError(f"Unsafe archive member path: {name!r}")
    normalized = name[2:] if name.startswith("./") else name
    parts = normalized.rstrip("/").split("/")
    if normalized not in ("", ".") and any(part in ("", ".", "..") for part in parts):
        raise BuildError(f"Unsafe archive member path: {name!r}")
    return normalized.rstrip("/")


def extract_installer_archives(installer: Path, destination: Path,
                               spec: InstallerSpec = OFFICIAL_INSTALLER) -> dict:
    # Verify before parsing. The fixed offset identifies this one installer;
    # no shell header is interpreted and no embedded program is launched.
    verify_installer(installer, spec)
    archives = {}
    seen = set()
    try:
        destination.mkdir(parents=True, exist_ok=False)
        with installer.open("rb") as source:
            source.seek(spec.payload_offset)
            with tarfile.open(fileobj=source, mode="r|gz") as archive:
                for member in archive:
                    normalized = safe_member_name(member.name)
                    if normalized in seen:
                        raise BuildError(f"Duplicate installer member: {member.name}")
                    seen.add(normalized)
                    if not member.isfile() and not member.isdir():
                        raise BuildError(f"Linked or special installer member: {member.name}")
                    if normalized in {"data/core.ung", "data/heaven.ung"} and member.name not in ARCHIVE_MEMBERS:
                        raise BuildError(f"Noncanonical installer archive path: {member.name}")
                    if member.name not in ARCHIVE_MEMBERS:
                        continue
                    if not member.isfile() or not 0 < member.size <= MAX_UNG_BYTES:
                        raise BuildError(f"Invalid installer archive member: {member.name}")
                    key = ARCHIVE_MEMBERS[member.name]
                    target = destination / f"{key}.ung"
                    payload = archive.extractfile(member)
                    if payload is None:
                        raise BuildError(f"Unreadable installer archive: {member.name}")
                    with payload, target.open("xb") as output:
                        shutil.copyfileobj(payload, output, CHUNK_SIZE)
                    if target.stat().st_size != member.size:
                        raise BuildError(f"Truncated installer archive: {member.name}")
                    archives[key] = {"path": str(target), "bytes": member.size,
                                     "sha256": sha256_file(target)}
        if set(archives) != {"core", "heaven"}:
            raise BuildError("Installer is missing data/core.ung or data/heaven.ung")
    except (OSError, tarfile.TarError, EOFError) as error:
        raise BuildError(f"Invalid installer payload: {error}") from error
    return archives


def _validate_asset_zip(path: Path) -> dict:
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        if len(names) != len(set(names)):
            raise BuildError("Packed assets contain duplicate ZIP entries")
        for entry in entries:
            if safe_member_name(entry.filename) != entry.filename or entry.is_dir():
                raise BuildError(f"Invalid packed asset path: {entry.filename}")
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise BuildError(f"Linked packed asset: {entry.filename}")
        bad = archive.testzip()
        if bad is not None:
            raise BuildError(f"Packed asset failed its CRC check: {bad}")
        if "heavenbench-assets.json" not in names:
            raise BuildError("Packed assets are missing heavenbench-assets.json")
        if archive.getinfo("heavenbench-assets.json").file_size > 16 * 1024 * 1024:
            raise BuildError("Packed asset manifest is unexpectedly large")
        manifest = json.loads(archive.read("heavenbench-assets.json"))
        if not isinstance(manifest, dict) or type(manifest.get("schema_version")) is not int or \
                manifest["schema_version"] != 1 or \
                manifest.get("format") != "heavenbench-assets" or \
                manifest.get("data_root_prefix") != "heaven" or \
                type(manifest.get("entry_count")) is not int or manifest["entry_count"] != len(names) - 1:
            raise BuildError("Packed asset manifest does not describe a complete base pack")
        required = {
            "heaven/heaven/heaven.world",
            "heaven/heaven/textures/ruins/ruins_ground_n.dds",
            "heaven/heaven/generated/ground/ruins_ground_h.dds",
            "heaven/sky/moon_disk.png", "heaven/sky/moon_normal.png", "heaven/sky/milkyway.png",
            "heaven/sky/README.md", "heaven/sky/LICENSE.md",
            "heaven/loading/loading_screen.png",
            "heaven/attribution/DeepBump.md", "heaven/attribution/bcdec.md",
        }
        required.update(REQUIRED_FEATURE_ASSETS)
        missing = required.difference(names)
        empty = {name for name in required - missing if archive.getinfo(name).file_size == 0}
        if missing or empty:
            raise BuildError(f"Packed assets have missing or empty runtime resources: {sorted(missing | empty)}")
        derived = manifest.get("derived_assets")
        if not isinstance(derived, list) or len(derived) != 1 or not isinstance(derived[0], dict):
            raise BuildError("Packed assets are missing ground-relief provenance")
        ground = derived[0]
        if ground.get("source_path") != "heaven/textures/ruins/ruins_ground_n.dds" or \
                ground.get("output_path") != "heaven/generated/ground/ruins_ground_h.dds" or \
                not isinstance(ground.get("generator_id"), str) or not ground["generator_id"] or \
                type(ground.get("generator_version")) is not int or ground["generator_version"] < 1 or \
                ground.get("attribution") != "attribution/DeepBump.md" or \
                ground.get("codec_attribution") != "attribution/bcdec.md" or \
                any(not isinstance(ground.get(key), str) or not re.fullmatch(r"[0-9a-f]{16}", ground[key])
                    for key in ("source_fingerprint_fnv1a64", "output_fingerprint_fnv1a64")):
            raise BuildError("Packed ground-relief provenance is incomplete")
        return {"entries": len(names), "manifest": manifest}


def validate_asset_zip(path: Path) -> dict:
    try:
        return _validate_asset_zip(path)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        raise BuildError(f"Invalid packed asset archive: {error}") from error


def run_command(command: list[str], *, log: Path, cwd: Path, environment: dict | None = None) -> None:
    with log.open("wb") as output:
        result = subprocess.run(command, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.STDOUT, check=False)
    if result.returncode != 0:
        raise BuildError(f"{Path(command[0]).name} failed with exit {result.returncode}; see {log}")


def usable_binary(path: Path) -> Path:
    path = path.expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise BuildError(f"Executable tool not found: {path}")
    return path


def prepare_tools(args: argparse.Namespace, repo_root: Path, work: Path, logs: Path) -> tuple[Path, Path]:
    build_root = Path(os.environ.get("HEAVEN_BUILD_ROOT", str(repo_root / "build"))).expanduser().absolute()
    suffix = ".exe" if os.name == "nt" else ""
    extractor = args.extractor
    packer = args.packer
    if args.skip_build:
        extractor = extractor or build_root / "tools" / f"uniginex{suffix}"
        packer = packer or build_root / "asset-tools" / f"heaven_asset_packer{suffix}"
    if extractor is not None:
        extractor = usable_binary(extractor)
    if packer is not None:
        packer = usable_binary(packer)
    if extractor is None or packer is None:
        if not sys.platform.startswith("linux"):
            raise BuildError("Automatic tool builds require Linux; supply --extractor and --packer on this platform")
        print("==> Preparing pinned CPU asset tools", flush=True)
        run_command([str(repo_root / "scripts/fetch_dependencies.sh")],
                    log=logs / "dependencies.log", cwd=repo_root)
        if extractor is None:
            tool_build = work / "build" / "tools"
            run_command([
                "cmake", "-S", str(repo_root), "-B", str(tool_build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_C_COMPILER={os.environ.get('HEAVEN_C_COMPILER', 'clang')}",
                f"-DCMAKE_CXX_COMPILER={os.environ.get('HEAVEN_CXX_COMPILER', 'clang++')}",
                "-DCMAKE_CXX_FLAGS=-stdlib=libc++",
            ], log=logs / "extractor-configure.log", cwd=repo_root)
            run_command(["cmake", "--build", str(tool_build), "--target", "uniginex",
                         "--parallel", str(args.jobs)], log=logs / "extractor-build.log", cwd=repo_root)
            extractor = usable_binary(tool_build / "uniginex")
        if packer is None:
            tool_build = work / "build" / "asset-tools"
            run_command([str(repo_root / "scripts/build_asset_tools.sh"), "--build-dir", str(tool_build),
                         "--jobs", str(args.jobs)], log=logs / "packer-build.log", cwd=repo_root)
            packer = usable_binary(tool_build / "heaven_asset_packer")
    return extractor, packer


def run_pipeline(args: argparse.Namespace, *, spec: InstallerSpec = OFFICIAL_INSTALLER,
                 repo_root: Path = REPO_ROOT) -> dict:
    repo_root = repo_root.resolve()
    work = args.work_dir.expanduser().resolve()
    output = args.output.expanduser().absolute()
    installer = select_installer(args.installer, repo_root, Path.cwd())
    if output.suffix != ".zip":
        raise BuildError("--output must name a .zip archive")
    if output.is_symlink() or (output.exists() and not output.is_file()):
        raise BuildError(f"Output must be a regular file, not a symlink or directory: {output}")
    if output.exists() and not args.overwrite:
        raise BuildError(f"Output already exists; use --overwrite to replace it: {output}")
    protected = [installer, *[path for path in (args.extractor, args.packer) if path is not None]]
    if any(output.resolve() == path.expanduser().resolve() for path in protected):
        raise BuildError("Output must not replace the installer or an executable tool")
    work.mkdir(parents=True, exist_ok=True)
    run_name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]
    logs = work / "runs" / run_name
    logs.mkdir(parents=True)
    print(f"==> Verifying installer: {installer}", flush=True)
    installer_info = acquire_installer(installer, no_download=args.no_download, spec=spec)
    extractor, packer = prepare_tools(args, repo_root, work, logs)
    output.parent.mkdir(parents=True, exist_ok=True)
    receipt_path = logs / "provenance.json"
    published = False
    try:
        with tempfile.TemporaryDirectory(prefix="stage-", dir=work, ignore_cleanup_errors=True) as stage_name, \
                tempfile.TemporaryDirectory(prefix=".heaven-assets-", dir=output.parent,
                                            ignore_cleanup_errors=True) as target_name:
            stage = Path(stage_name)
            print("==> Reading core.ung and heaven.ung from the verified installer", flush=True)
            archives = extract_installer_archives(installer, stage / "archives", spec)
            for key in ("core", "heaven"):
                print(f"==> Extracting {key}.ung", flush=True)
                run_command([str(extractor), "-o", archives[key]["path"], str(stage / "extracted" / key)],
                            log=logs / f"extract-{key}.log", cwd=stage)
            temporary_zip = Path(target_name) / "assets.zip"
            environment = os.environ.copy()
            resource_override = environment.get("HEAVEN_RESOURCE_DIR")
            resource_directory = (Path(resource_override).expanduser().resolve() if resource_override
                                  else repo_root / "benchmark/assets")
            environment["HEAVEN_RESOURCE_DIR"] = str(resource_directory)
            command = [str(packer), "--data", str(stage / "extracted/heaven"),
                       "--pack-assets", str(temporary_zip), "--clouds"]
            print("==> Packing runtime assets and generated ground relief", flush=True)
            run_command(command, log=logs / "packer.log", cwd=stage, environment=environment)
            print("==> Validating runtime archive", flush=True)
            validation = validate_asset_zip(temporary_zip)
            receipt = {
                "schema_version": 1,
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "installer": {"path": str(installer), **installer_info, "pin": asdict(spec)},
                "archives": {key: {k: v for k, v in info.items() if k != "path"}
                             for key, info in archives.items()},
                "tools": {key: {"path": str(path), "sha256": sha256_file(path)}
                          for key, path in (("extractor", extractor), ("packer", packer))},
                "resource_directory": environment["HEAVEN_RESOURCE_DIR"],
                "pack_options": ["--clouds"],
                "output": {"path": str(output), "bytes": temporary_zip.stat().st_size,
                           "sha256": sha256_file(temporary_zip), **validation},
            }
            # Prepare the receipt before the final publication, so a metadata
            # write error also leaves any pre-existing output untouched.
            with receipt_path.open("x", encoding="utf-8") as receipt_file:
                json.dump(receipt, receipt_file, indent=2)
                receipt_file.write("\n")
                receipt_file.flush()
                os.fsync(receipt_file.fileno())
            publish_file(temporary_zip, output, overwrite=args.overwrite)
            published = True
    finally:
        if not published:
            receipt_path.unlink(missing_ok=True)
    print(f"==> Runtime assets: {output}\n==> Provenance: {receipt_path}", flush=True)
    return receipt


def positive_int(value: str) -> int:
    try:
        result = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if result < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def create_parser(repo_root: Path = REPO_ROOT) -> argparse.ArgumentParser:
    build_root = Path(os.environ.get("HEAVEN_BUILD_ROOT", str(repo_root / "build")))
    parser = argparse.ArgumentParser(
        description="Build HeavenBench runtime assets from the verified official Heaven 4.0 installer.",
        epilog="Requires Python 3.10+. Linux can build the CPU tools automatically; "
               "other platforms require existing --extractor and --packer executables.",
    )
    parser.add_argument("--installer", type=Path, help="Use this installer, or download it here if absent")
    parser.add_argument("--work-dir", type=Path, default=build_root / "asset-builder",
                        help="Build, staging, and provenance directory (default: build/asset-builder)")
    parser.add_argument("--output", type=Path, default=repo_root / "assets/assets.zip",
                        help="Final runtime ZIP (default: assets/assets.zip)")
    parser.add_argument("--extractor", type=Path, help="Use an existing uniginex executable")
    parser.add_argument("--packer", type=Path, help="Use an existing heaven_asset_packer executable")
    parser.add_argument("--skip-build", action="store_true", help="Require existing tools, using canonical build paths by default")
    parser.add_argument("--jobs", type=positive_int, default=2, help="Parallel compiler jobs (default: 2)")
    parser.add_argument("--overwrite", action="store_true", help="Replace an existing output only after complete validation")
    parser.add_argument("--no-download", action="store_true", help="Fail if the selected installer is absent")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = create_parser().parse_args(argv)
    try:
        run_pipeline(args, spec=OFFICIAL_INSTALLER)
    except (BuildError, OSError, ValueError, zipfile.BadZipFile, RuntimeError) as error:
        print(f"Asset build failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Asset build interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
