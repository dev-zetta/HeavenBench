#!/usr/bin/env python3
"""Keep local tooling/original Heaven inputs out of tracked release sources."""

import fnmatch
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import unittest
import zipfile


REPOSITORY = Path(__file__).resolve().parents[1]


def original_asset_members(archive):
    """Recognize base packs even when renamed; extension aliases copy no data."""
    names = archive.namelist()
    original = [name for name in names
                if PurePosixPath(name).suffix.lower() == ".ung" or any(
                    fnmatch.fnmatchcase(PurePosixPath(name).name.lower(), pattern)
                    for pattern in ("unigine_heaven-*.run", "unigine_heaven-*.exe",
                                    "unigine_heaven-*.dmg"))]
    if "heavenbench-assets.json" not in names:
        return original
    info = archive.getinfo("heavenbench-assets.json")
    if info.file_size > 8 * 1024 * 1024:
        raise ValueError("Asset manifest is unexpectedly large")
    manifest = json.loads(archive.read(info))
    if manifest.get("format") != "heavenbench-assets":
        raise ValueError("Invalid Heaven asset manifest")
    prefix = manifest.get("data_root_prefix")
    if not isinstance(prefix, str) or not prefix or "/" in prefix or prefix in {".", ".."}:
        raise ValueError("Invalid Heaven data root prefix")
    # Generated packs contain texture_sets/generated entries and their root
    # manifest. Original-path aliases in metadata refer to the separate base
    # pack and are allowed; copying original payloads into this ZIP is not.
    generated_prefixes = (f"{prefix}/texture_sets/", f"{prefix}/generated/",
                          f"{prefix}/heaven/generated/")
    original.extend(name for name in names
                    if name != "heavenbench-assets.json" and not name.endswith("/")
                    and not name.startswith(generated_prefixes))
    return sorted(set(original))


class IgnorePolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("git"):
            raise unittest.SkipTest("Git is required to verify ignore semantics")
        cls.temporary = tempfile.TemporaryDirectory(prefix="heaven-git-hygiene-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        shutil.copyfile(REPOSITORY / ".gitignore", cls.directory / ".gitignore")
        subprocess.run(["git", "init", "--quiet", str(cls.directory)], check=True)

    def ignored(self, paths):
        result = subprocess.run(
            ["git", "-c", "core.excludesFile=/dev/null", "check-ignore",
             "--no-index", "--stdin", "-z"], cwd=self.directory,
            input="\0".join(paths) + "\0", text=True, capture_output=True, check=False)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        return set(result.stdout.rstrip("\0").split("\0")) if result.stdout else set()

    def tracked_paths(self):
        result = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=REPOSITORY,
                                capture_output=True, text=True, check=False)
        if result.returncode or Path(result.stdout.strip()).resolve() != REPOSITORY:
            self.skipTest("Source archive has no repository index to audit")
        result = subprocess.run(["git", "ls-files", "-z"], cwd=REPOSITORY,
                                capture_output=True, text=True, check=True)
        return [path for path in result.stdout.split("\0") if path]

    def test_local_inputs_and_tooling_are_ignored(self):
        paths = {
            "data/Unigine_Heaven-4.0.run", "data/data/heaven.ung",
            "extracted/heaven/heaven/heaven.world", "assembled/scene.obj",
            "downloads/Unigine_Heaven-4.0.run", "Unigine_Heaven-4.0.exe",
            "downloads/Unigine_Heaven-4.0.dmg", "elsewhere/core.ung", "CORE.UNG",
            ".Unigine_Heaven-4.0.run.fixture.part",
            "downloads/.Unigine_Heaven-4.0.run.fixture.part",
            "assets.zip", "assets/assets.zip", "assets/assets-hires.zip",
            "cache/tessellation.zip", "assets/cache/tessellation.zip", "imgui.ini",
            "benchmark/imgui.ini",
            "results/heavenbench.json", "results/heavenbench.html",
            "profiles/capture.json", "profiles/capture.html.tmp",
            "loading-results/summary.json",
            "assets/.heaven-assets-fixture/assets.zip", "output/.heaven-assets-fixture/assets.zip",
            "build/renderer/heaven_benchmark", "benchmark/build-android-arm64/binary",
            "benchmark/build-debug/CMakeCache.txt", "benchmark/build-ci/binary",
            "android/build/generated/source.txt",
            "android/app/build/outputs/apk/debug/app-debug.apk", "compile_commands.json",
            ".gradle/caches/state", "android/.gradle/caches/state",
            "android/local.properties", "android/heavenbench.iml",
            ".kiro/specs/private/design.md", ".vscode/settings.json", ".idea/workspace.xml",
            ".vs/Project/config/applicationhost.config",
            ".codex/config.toml", ".agents/state.json", "tools/.codex/config.toml",
            "tools/.vscode/settings.json", "AGENTS.md", "tools/extraction/AGENTS.md",
            ".aws/credentials", ".aws/config", "tools/.aws/credentials",
            ".DS_Store", "docs/.DS_Store", "Thumbs.db", "benchmark/Thumbs.db",
            ".venv/bin/python", "tools/.venv/bin/python",
            ".pytest_cache/v/cache/nodeids", "tools/.pytest_cache/v/cache/nodeids",
            "scripts/__pycache__/helper.cpython-312.pyc", "tools/helper.pyc",
        }
        self.assertEqual(self.ignored(paths), paths)

    def test_authored_generated_and_fixture_files_remain_eligible(self):
        paths = {
            "benchmark/assets/sky/moon_disk.png", "benchmark/assets/clouds/eve1.dds",
            "benchmark/assets/loading/loading_screen.png", "packaging/linux/heavenbench.svg",
            "benchmark/materials/heaven_lit.filamat", "benchmark/src/rhi/materials_data.cpp",
            "assets/generated/high-resolution.zip", "assets/generated/normal.dds",
            "assets/generated/texture-set.json",
            "tests/fixtures/example.zip", "tests/fixtures/generated-texture-pack.zip",
            "tests/fixtures/authored.obj", "tests/data/example.json",
            "tests/fixtures/compile_commands.json", "config.json", "CHANGELOG.md",
            "docs/RELEASE_READINESS.md", "docs/tutorial/build/example.cpp",
            "tools/extraction/uniginex.cpp", "scripts/build_asset_tools.sh",
        }
        self.assertEqual(self.ignored(paths), set())

    def test_index_contains_no_ignored_local_files(self):
        ignored = self.ignored(self.tracked_paths())
        self.assertFalse(ignored, "Untrack these files while retaining local copies:\n" +
                         "\n".join(sorted(ignored)))

    def test_tracked_zip_files_do_not_contain_base_assets(self):
        for relative in self.tracked_paths():
            if Path(relative).suffix.lower() != ".zip":
                continue
            with self.subTest(archive=relative), zipfile.ZipFile(REPOSITORY / relative) as archive:
                self.assertEqual(original_asset_members(archive), [],
                                 "Original Heaven payloads must remain in a local base pack")


class AssetArchivePolicyTest(unittest.TestCase):
    def members(self, files):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            for name, contents in files.items():
                archive.writestr(name, contents)
        data.seek(0)
        with zipfile.ZipFile(data) as archive:
            return original_asset_members(archive)

    @staticmethod
    def manifest():
        return json.dumps({"format": "heavenbench-assets", "schema_version": 2,
                           "data_root_prefix": "heaven"})

    def test_renamed_base_or_mixed_pack_is_rejected(self):
        files = {"heavenbench-assets.json": self.manifest(),
                 "heaven/texture_sets/4k/new.dds": b"generated fixture",
                 "heaven/heaven/textures/original.dds": b"original fixture"}
        self.assertEqual(self.members(files), ["heaven/heaven/textures/original.dds"])

    def test_generated_only_pack_with_external_aliases_is_allowed(self):
        metadata = json.dumps({"entries": [{"status": "alias", "variant": {
            "archive_path": "core/core/textures/mesh_base_diffuse.dds"}}]})
        files = {"heavenbench-assets.json": self.manifest(),
                 "heaven/texture_sets/texture-set.json": metadata,
                 "heaven/texture_sets/4k/new.dds": b"generated fixture",
                 "heaven/texture_sets/metadata/contact-sheet.png": b"generated fixture"}
        self.assertEqual(self.members(files), [])

    def test_original_ung_is_rejected_even_without_asset_manifest(self):
        self.assertEqual(self.members({"download/HEAVEN.UNG": b"original fixture"}),
                         ["download/HEAVEN.UNG"])

    def test_generated_ground_texture_pack_is_allowed(self):
        files = {"heavenbench-assets.json": self.manifest(),
                 "heaven/heaven/generated/ground/ruins_ground_h.dds": b"generated fixture"}
        self.assertEqual(self.members(files), [])

    def test_wrapped_original_installer_is_rejected(self):
        name = "downloads/Unigine_Heaven-4.0.run"
        self.assertEqual(self.members({name: b"installer fixture"}), [name])


if __name__ == "__main__":
    unittest.main()
