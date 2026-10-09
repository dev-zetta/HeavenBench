#!/usr/bin/env python3
"""Verify packaged startup from an unrelated working directory, without a GPU."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(sys.argv.pop(1)).resolve()
REPOSITORY = Path(__file__).resolve().parents[2]


class RuntimePathsCliTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heaven-runtime-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cwd = self.root / "unrelated working directory"
        self.cwd.mkdir()
        self.user = self.root / "writable user data"
        self.user.mkdir()
        self.env = os.environ.copy()
        for name in ("HEAVEN_USER_DIR", "HEAVEN_RESOURCE_DIR", "HEAVEN_DEFAULT_DATA",
                     "HEAVEN_DATA", "APPDIR", "APPIMAGE"):
            self.env.pop(name, None)
        self.command = [str(BINARY)]

    def effective_config(self, *arguments):
        output = self.cwd / "effective.json"
        result = subprocess.run(
            [*self.command, *arguments, "--write-config", str(output)],
            cwd=self.cwd, env=self.env, capture_output=True, text=True, timeout=20,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(output.read_text(encoding="utf-8"))

    def test_user_config_and_cache_are_independent_of_working_directory(self):
        self.env["HEAVEN_USER_DIR"] = str(self.user)
        (self.user / "config.json").write_text(
            json.dumps({"width": 913, "tessellation.cache_file": ""}), encoding="utf-8"
        )
        config = self.effective_config()
        self.assertEqual(config["width"], 913)
        cache = Path(config["tessellation.cache_file"])
        self.assertEqual(cache.parent, self.user / "cache")
        self.assertRegex(cache.name, r"^tessellation-v\d+\.zip$")
        self.assertFalse((self.cwd / "config.json").exists())

    def test_explicit_cli_files_override_user_defaults(self):
        self.env["HEAVEN_USER_DIR"] = str(self.user)
        (self.user / "config.json").write_text('{"width":913}', encoding="utf-8")
        (self.cwd / "chosen.json").write_text('{"width":1001}', encoding="utf-8")
        config = self.effective_config(
            "--config", "chosen.json", "--width", "1234",
            "--data", "relative assets.zip", "--tess-cache", "chosen-cache.zip",
        )
        self.assertEqual(config["width"], 1234)
        self.assertEqual(config["data_dir"], "relative assets.zip")
        self.assertEqual(config["tessellation.cache_file"], "chosen-cache.zip")

    def test_unset_override_keeps_development_defaults(self):
        (self.cwd / "config.json").write_text('{"width":913}', encoding="utf-8")
        config = self.effective_config()
        self.assertNotEqual(config["width"], 913)
        self.assertEqual(config["tessellation.cache_file"], "")
        self.assertEqual(config["data_dir"], "assets")

    def prepare_launcher(self):
        appdir = self.root / "relocated AppDir"
        binary = appdir / "usr/bin/heaven_benchmark"
        binary.parent.mkdir(parents=True)
        binary.symlink_to(BINARY)
        image = self.root / "image directory" / "HeavenBench.AppImage"
        image.parent.mkdir()
        self.env.update(APPDIR=str(appdir), APPIMAGE=str(image), HEAVEN_USER_DIR=str(self.user))
        self.command = [str(REPOSITORY / "packaging/linux/AppRun")]
        return image.parent

    @unittest.skipUnless(sys.platform == "linux", "AppRun is a Linux launcher")
    def test_launcher_discovers_loose_assets_beside_image(self):
        image_directory = self.prepare_launcher()
        assets = image_directory / "assets"
        (assets / "heaven").mkdir(parents=True)
        (assets / "heaven/heaven.world").write_bytes(b"fixture")
        config = self.effective_config()
        self.assertEqual(config["data_dir"], str(assets))

    @unittest.skipUnless(sys.platform == "linux", "AppRun is a Linux launcher")
    def test_launcher_saved_data_overrides_discovered_default(self):
        image_directory = self.prepare_launcher()
        (image_directory / "assets.zip").write_bytes(b"fixture")
        (self.user / "config.json").write_text(
            '{"data_dir":"saved-custom-assets.zip"}', encoding="utf-8"
        )
        config = self.effective_config()
        self.assertEqual(config["data_dir"], "saved-custom-assets.zip")

    @unittest.skipUnless(sys.platform == "linux", "AppRun is a Linux launcher")
    def test_launcher_discovers_zip_directory(self):
        image_directory = self.prepare_launcher()
        assets = image_directory / "assets"
        assets.mkdir()
        (assets / "custom-pack.zip").write_bytes(b"fixture")
        self.assertEqual(self.effective_config()["data_dir"], str(assets))

    @unittest.skipUnless(sys.platform == "linux", "AppRun is a Linux launcher")
    def test_launcher_explicit_data_precedence_and_relative_config(self):
        self.prepare_launcher()
        self.env["HEAVEN_DATA"] = "environment-assets.zip"
        (self.user / "config.json").write_text('{"data_dir":"saved-assets.zip"}', encoding="utf-8")
        self.assertEqual(self.effective_config()["data_dir"], "environment-assets.zip")
        (self.cwd / "chosen.json").write_text('{"width":1001}', encoding="utf-8")
        config = self.effective_config("--config", "chosen.json", "--data", "cli-assets.zip")
        self.assertEqual(config["data_dir"], "cli-assets.zip")
        self.assertEqual(config["width"], 1001)

    @unittest.skipUnless(sys.platform == "linux", "AppRun is a Linux launcher")
    def test_launcher_user_asset_fallback(self):
        self.prepare_launcher()
        config = self.effective_config()
        self.assertEqual(config["data_dir"], str(self.user / "assets.zip"))


if __name__ == "__main__":
    unittest.main()
