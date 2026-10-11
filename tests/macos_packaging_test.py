#!/usr/bin/env python3
"""Exercise Mach-O deployment/import parsing and hostile package inputs off-device."""

import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bundle_macos as bundle
import collect_licenses
import verify_macos as verify


class MacOsPackagingTest(unittest.TestCase):
    @unittest.skipUnless(os.name == "posix", "Homebrew symlink fixture requires POSIX")
    def test_sdl_compat_alias_seeds_exact_runtime_and_retains_notices(self):
        with tempfile.TemporaryDirectory(prefix="heaven SDL keg ") as temporary:
            root = Path(temporary).resolve()
            cellar = root / "Cellar"
            compat = cellar / "sdl2-compat/2.32.72"
            classic = cellar / "sdl2/2.32.10"
            sdl3 = cellar / "sdl3/3.4.16"
            for prefix in (compat, classic, sdl3):
                (prefix / "lib").mkdir(parents=True)
                (prefix / "INSTALL_RECEIPT.json").write_text("{}")
            library = sdl3 / "lib/libSDL3.0.16.dylib"
            library.write_bytes(b"isolated runtime fixture")
            (sdl3 / "lib/libSDL3.dylib").symlink_to(library.name)
            notice = b"SDL3 fixture redistributed notice\n"
            (sdl3 / "LICENSE.txt").write_bytes(notice)
            alias = root / "opt/sdl2"
            alias.parent.mkdir()
            alias.symlink_to(compat, target_is_directory=True)
            with mock.patch.object(bundle, "run", return_value=str(sdl3)) as brew:
                self.assertEqual(bundle.sdl_runtime_seeds(classic), [])
                brew.assert_not_called()
                seeds = bundle.sdl_runtime_seeds(alias)
                brew.assert_called_once_with(["brew", "--prefix", "sdl3"])
            self.assertEqual(seeds, [(sdl3 / "lib/libSDL3.dylib", "libSDL3.dylib")])
            app = root / "fixture app/HeavenBench.app"
            (app / "Contents/Frameworks").mkdir(parents=True)
            cellar_alias = root / "Cellar alias"
            cellar_alias.symlink_to(cellar, target_is_directory=True)
            bundler = bundle.Bundler(app, cellar_alias, [], {})
            installed = bundler.add_library(*seeds[0])
            self.assertEqual(bundler.cellar, cellar)
            self.assertEqual(installed.name, "libSDL3.dylib")
            self.assertFalse(installed.is_symlink())
            self.assertEqual(installed.read_bytes(), library.read_bytes())
            self.assertEqual(bundler.records[0]["formula"], "sdl3")
            self.assertEqual(bundler.records[0]["version"], "3.4.16")
            self.assertEqual(bundler.records[0]["original_sha256"], bundle.sha256(library))
            outside = root / "not a keg/libSDL3.dylib"
            outside.parent.mkdir()
            outside.write_bytes(b"unowned library")
            with self.assertRaisesRegex(ValueError, "No license/provenance source"):
                bundler.provenance(outside)
            labels = [f"{formula}-{version}={keg}"
                      for (formula, version), keg in bundler.formulas.items()]
            output = root / "collected notices"
            collect_licenses.collect(output, labels)
            retained = output / "dependencies/sdl3-3.4.16/LICENSE.txt"
            self.assertEqual(retained.read_bytes(), notice)
            manifest = json.loads((output / "MANIFEST.json").read_text())
            self.assertIn({"path": retained.relative_to(output).as_posix(),
                           "sha256": bundle.sha256(retained)}, manifest["files"])

    def test_sdl_compat_missing_runtime_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="heaven missing SDL ") as temporary:
            root = Path(temporary)
            compat = root / "sdl2-compat/2.32.72"
            compat.mkdir(parents=True)
            sdl3 = root / "sdl3/3.4.16"
            sdl3.mkdir(parents=True)
            with mock.patch.object(bundle, "run", return_value=str(sdl3)), \
                    self.assertRaisesRegex(ValueError, "requires its SDL3 runtime"):
                bundle.sdl_runtime_seeds(compat)

    @unittest.skipUnless(os.name == "posix", "dyld symlink fixture requires POSIX")
    def test_sdl_dyld_inventory_accepts_alias_and_rejects_host_fallback(self):
        with tempfile.TemporaryDirectory(prefix="heaven SDL image paths ") as temporary:
            root = Path(temporary).resolve()
            frameworks = root / "HeavenBench.app/Contents/Frameworks"
            frameworks.mkdir(parents=True)
            sdl2 = frameworks / "libSDL2-2.0.0.dylib"
            sdl3 = frameworks / "libSDL3.dylib"
            host = root / "Homebrew/libSDL3.dylib"
            for library in (sdl2, sdl3, host):
                library.parent.mkdir(parents=True, exist_ok=True)
                library.write_bytes(b"image identity fixture")
            alias = root / "app alias"
            alias.symlink_to(frameworks, target_is_directory=True)
            images = [str(alias / sdl2.name), str(alias / sdl3.name), "/usr/lib/libSystem.B.dylib"]
            self.assertEqual(verify.validate_sdl_images(images, sdl2, sdl3),
                             {"libsdl2": sdl2.name, "libsdl3": sdl3.name})
            for invalid in ([str(sdl2), str(host)], [str(sdl2)],
                            [str(sdl2), str(sdl3), str(sdl3)]):
                with self.subTest(images=invalid), self.assertRaises(ValueError):
                    verify.validate_sdl_images(invalid, sdl2, sdl3)
            self.assertEqual(verify.validate_sdl_images([str(sdl2)], sdl2), {"libsdl2": sdl2.name})
            with self.assertRaisesRegex(ValueError, "unexpectedly loaded"):
                verify.validate_sdl_images([str(sdl2), str(sdl3)], sdl2)

    def test_sdl_probe_is_isolated_and_removes_caller_overrides(self):
        with tempfile.TemporaryDirectory(prefix="heaven SDL subprocess ") as temporary:
            root = Path(temporary).resolve()
            app = root / "HeavenBench.app"
            sdl2 = app / "Contents/Frameworks/libSDL2-2.0.0.dylib"
            sdl3 = sdl2.with_name("libSDL3.dylib")
            sdl2.parent.mkdir(parents=True)
            for library in (sdl2, sdl3):
                library.write_bytes(b"fixture")
            inventory = {"libraries": [{"formula": "sdl2-compat", "path": sdl2.relative_to(app).as_posix()}],
                         "dynamic_dependencies": [{"owner": "SDL2-compat", "formula": "sdl3",
                                                   "path": sdl3.relative_to(app).as_posix(),
                                                   "lookup": "@loader_path/libSDL3.dylib"}]}
            env = {"PATH": "/usr/bin:/bin", "HEAVEN_USER_DIR": "user with spaces",
                   "SDL2COMPAT_DYLIB_PATH": "host override", "SDL_DYNAMIC_API": "host override",
                   "SDL3_DYNAMIC_API": "host override", "SDL_VIDEODRIVER": "cocoa",
                   "SDL_HINT_VIDEO_DRIVER": "cocoa", "SDL2_OTHER_HINT": "host override",
                   "DYLD_LIBRARY_PATH": "host fallback"}
            response = {"api_version": [2, 32, 72], "driver": "dummy", "hidden_dummy_window": True,
                        "images": [str(sdl2), str(sdl3)]}
            with mock.patch.object(verify, "checked", return_value=json.dumps(response)) as child:
                result = verify.verify_sdl_runtime(app, inventory, root, env)
            command, cwd, child_env = child.call_args.args
            self.assertEqual(command, [sys.executable, "-c", verify.SDL_PROBE, sdl2])
            compile(command[2], "isolated SDL2 probe", "exec")
            self.assertEqual(cwd, root)
            self.assertEqual(child_env, {"PATH": "/usr/bin:/bin", "HEAVEN_USER_DIR": "user with spaces",
                                         "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
            self.assertEqual(result["loaded_runtime_files"], {"libsdl2": sdl2.name, "libsdl3": sdl3.name})
            with mock.patch.object(verify, "checked", side_effect=ValueError("child constructor exit")), \
                    self.assertRaisesRegex(ValueError, "constructor exit"):
                verify.verify_sdl_runtime(app, inventory, root, env)
            with self.assertRaisesRegex(ValueError, "inventory is incomplete"):
                verify.verify_sdl_runtime(app, {**inventory, "dynamic_dependencies": []}, root, env)

    @unittest.skipUnless(os.name == "posix", "shell launcher fixture requires POSIX")
    def test_resource_launcher_executes_with_spaces_aliases_args_and_env(self):
        with tempfile.TemporaryDirectory(prefix="heaven actual launcher ") as temporary:
            root = Path(temporary).resolve()
            parent = root / "actual apps with spaces"
            contents = parent / "HeavenBench.app/Contents"
            resources = contents / "Resources"
            macos = contents / "MacOS"
            for directory in (resources, macos):
                directory.mkdir(parents=True)
            launcher = resources / "launch-heavenbench.sh"
            launcher.write_text(bundle.LAUNCH_SCRIPT)
            benchmark = macos / "heaven_benchmark"
            benchmark.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                                 "print(json.dumps({'args':sys.argv[1:], 'env':dict(os.environ)}))\n")
            benchmark.chmod(0o755)
            alias = root / "alias app directory"
            alias.symlink_to(parent, target_is_directory=True)
            aliased = alias / "HeavenBench.app/Contents/Resources/launch-heavenbench.sh"
            home = root / "home with spaces"
            home.mkdir()
            env = {"PATH": "/usr/bin:/bin", "HOME": str(home)}

            def execute(arguments=(), extra=None):
                completed = subprocess.run(["/bin/sh", aliased, *arguments], cwd=root,
                                           env={**env, **(extra or {})}, text=True,
                                           capture_output=True, check=True, timeout=10)
                return json.loads(completed.stdout)

            args = ["--name", "spaces and 'quotes'", "$(must stay literal)", "", "--flag"]
            initial = execute(args)
            self.assertEqual(initial["args"], args)
            emitted = initial["env"]
            user = home / "Library/Application Support/HeavenBench"
            self.assertTrue(user.is_dir())
            self.assertEqual(emitted["HEAVEN_USER_DIR"], str(user))
            self.assertEqual(emitted["HEAVEN_DEFAULT_DATA"], str(user / "assets.zip"))
            self.assertEqual(emitted["HEAVEN_RESOURCE_DIR"], str(resources / "resources"))
            self.assertEqual(emitted["DYLD_LIBRARY_PATH"], str(contents / "Frameworks"))
            self.assertEqual(emitted["SDL_VULKAN_LIBRARY"], str(contents / "Frameworks/libvulkan.1.dylib"))
            self.assertEqual(emitted["VK_DRIVER_FILES"], str(resources / "vulkan/icd.d/MoltenVK_icd.json"))
            self.assertEqual(emitted["VK_ICD_FILENAMES"], emitted["VK_DRIVER_FILES"])
            (parent / "assets.zip").write_bytes(b"external discovery fixture")
            self.assertEqual(execute()["env"]["HEAVEN_DEFAULT_DATA"], str(parent / "assets.zip"))
            override = {"HEAVEN_DATA": "external env assets.zip", "HEAVEN_USER_DIR": str(root / "custom user")}
            self.assertEqual(execute(["--help"], override)["args"], ["--data", override["HEAVEN_DATA"], "--help"])
            self.assertEqual(execute(["--data", "explicit assets.zip"], override)["args"],
                             ["--data", "explicit assets.zip"])
            self.assertEqual(execute(extra={"HEAVEN_DEFAULT_DATA": "saved default"})["env"]["HEAVEN_DEFAULT_DATA"],
                             "saved default")

    @unittest.skipUnless(os.name == "posix", "ICD symlink fixture requires POSIX")
    def test_icd_containment_accepts_symlinked_temp_root_and_rejects_escape(self):
        with tempfile.TemporaryDirectory(prefix="heaven symlink temp ") as temporary:
            root = Path(temporary).resolve()
            actual = root / "actual"
            actual.mkdir()
            alias = root / "alias"
            alias.symlink_to(actual, target_is_directory=True)
            contents = alias / "HeavenBench.app/Contents"
            frameworks = contents / "Frameworks"
            frameworks.mkdir(parents=True)
            molten = frameworks / "libMoltenVK.dylib"
            molten.write_bytes(b"fixture")
            icd_path = contents / "Resources/vulkan/icd.d/MoltenVK_icd.json"
            icd_path.parent.mkdir(parents=True)
            icd_path.write_text(json.dumps({"ICD": {"library_path": "../../../Frameworks/libMoltenVK.dylib"}}))
            self.assertEqual(verify.resolve_icd_library(icd_path, frameworks), molten.resolve())

            outside = root / "outside.dylib"
            outside.write_bytes(b"must remain outside bundle")
            molten.unlink()
            molten.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, "escapes bundled Frameworks"):
                verify.resolve_icd_library(icd_path, frameworks)

    def test_minimum_uses_deployment_not_sdk_or_dylib_version(self):
        commands = """Load command 0
          cmd LC_BUILD_VERSION
      cmdsize 32
     platform 1
        minos 14.0
          sdk 26.0
Load command 1
          cmd LC_ID_DYLIB
      current version 21.1.8
Load command 2
          cmd LC_VERSION_MIN_MACOSX
      version 15.2
          sdk 26.0
"""
        self.assertEqual(bundle.minimum_macos(commands), "15.2")
        self.assertEqual(bundle.version_tuple("14.0"), bundle.version_tuple("14.0.0"))
        with self.assertRaisesRegex(ValueError, "minimum-version"):
            bundle.minimum_macos("cmd LC_ID_DYLIB\n version 21.1.8\n")

    def test_imports_preserve_paths_with_spaces(self):
        text = "/some app/heaven_benchmark:\n\t@rpath/libc++.1.dylib (compatibility version 1.0.0, current version 1.0.0)\n\t/a path/libSDL2.dylib (compatibility version 1.0.0, current version 2.0.0)\n"
        self.assertEqual(bundle.dependencies(text), ["@rpath/libc++.1.dylib", "/a path/libSDL2.dylib"])

    def test_rpath_resolution_uses_owner_before_global_search(self):
        with tempfile.TemporaryDirectory(prefix="heaven macho paths ") as temporary:
            root = Path(temporary)
            own = root / "own libs"
            other = root / "other libs"
            own.mkdir()
            other.mkdir()
            for directory in (own, other):
                (directory / "libExample.dylib").write_bytes(b"fixture")
            owner = root / "binary"
            selected = bundle.resolve_import("@rpath/libExample.dylib", owner, root,
                                             [other], ["@loader_path/own libs"])
            self.assertEqual(selected, (own / "libExample.dylib").resolve())
            with self.assertRaisesRegex(ValueError, "Unresolved"):
                bundle.resolve_import("@rpath/missing.dylib", owner, root, [other], [])

    def test_release_rejects_wrong_architecture_and_debug(self):
        good = {"CMAKE_BUILD_TYPE": "Release", "CMAKE_OSX_ARCHITECTURES": "arm64",
                "HEAVEN_TARGET_PLATFORM": "macos", "HEAVEN_CPU_ARCH": "portable",
                "CMAKE_OSX_DEPLOYMENT_TARGET": "14.0"}
        bundle.require_release(good)
        for name, value in (("CMAKE_BUILD_TYPE", "Debug"), ("CMAKE_OSX_ARCHITECTURES", "x86_64"),
                            ("CMAKE_OSX_DEPLOYMENT_TARGET", "13.0")):
            with self.subTest(name=name), self.assertRaises(ValueError):
                bundle.require_release({**good, name: value})

    def check_zip(self, names):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            for name in names:
                archive.writestr(name, b"fixture")
        data.seek(0)
        with zipfile.ZipFile(data) as archive:
            return verify.safe_members(archive)

    def test_zip_rejects_traversal_absolute_and_wrong_root(self):
        self.assertEqual(self.check_zip(["HeavenBench.app/Contents/Info.plist"]),
                         {"HeavenBench.app/Contents/Info.plist"})
        for name in ("HeavenBench.app/../outside", "/HeavenBench.app/file", "other.app/file",
                     "HeavenBench.app/Contents\\escape"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Unsafe"):
                self.check_zip([name])

    def test_zip_rejects_links_and_empty_archives(self):
        data = io.BytesIO()
        entry = zipfile.ZipInfo("HeavenBench.app/Contents/escape")
        entry.create_system = 3
        entry.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr(entry, "/outside")
        data.seek(0)
        with zipfile.ZipFile(data) as archive, self.assertRaisesRegex(ValueError, "symlink"):
            verify.safe_members(archive)
        with self.assertRaisesRegex(ValueError, "Empty"):
            self.check_zip([])


if __name__ == "__main__":
    unittest.main()
