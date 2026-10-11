#!/usr/bin/env python3
"""Exercise Mach-O deployment/import parsing and hostile package inputs off-device."""

import io
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bundle_macos as bundle
import verify_macos as verify


class MacOsPackagingTest(unittest.TestCase):
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
