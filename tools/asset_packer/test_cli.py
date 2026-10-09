#!/usr/bin/env python3
"""Check the standalone packer's entry point without assets or a display."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class PackerCliTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="heaven-packer-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.environment = os.environ.copy()
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "HEAVEN_DEFAULT_DATA",
                    "HEAVEN_USER_DIR", "HEAVEN_RESOURCE_DIR"):
            self.environment.pop(key, None)

    def run_tool(self, *arguments):
        return subprocess.run(
            [str(BINARY), *arguments], cwd=self.directory, env=self.environment,
            text=True, capture_output=True, check=False, timeout=20,
        )

    def test_help_needs_no_display(self):
        result = self.run_tool("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("heaven_asset_packer", result.stdout)
        self.assertFalse((self.directory / "assets").exists())

    def test_invalid_option_prevents_writes(self):
        result = self.run_tool("--unknown-packer-option")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.directory / "assets").exists())

    def test_missing_source_keeps_target_absent(self):
        result = self.run_tool("--data", "missing", "--pack-assets", "result.zip")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse((self.directory / "result.zip").exists())

    def test_configuration_precedence_matches_renderer(self):
        self.environment["HEAVEN_DEFAULT_DATA"] = "environment-default"
        (self.directory / "settings.json").write_text(
            json.dumps({"data_dir": "configured", "music.enabled": True}),
            encoding="utf-8",
        )
        result = self.run_tool(
            "--config", "settings.json", "--data", "selected", "--no-music",
            "--write-config", "resolved.json",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        resolved = json.loads((self.directory / "resolved.json").read_text())
        self.assertEqual(resolved["data_dir"], "selected")
        self.assertFalse(resolved["music.enabled"])
        self.assertFalse((self.directory / "assets").exists())

    def test_missing_explicit_config_is_fatal(self):
        result = self.run_tool("--config", "missing.json", "--pack-assets", "result.zip")
        self.assertEqual(result.returncode, 1)
        self.assertFalse((self.directory / "result.zip").exists())


if __name__ == "__main__":
    unittest.main()
