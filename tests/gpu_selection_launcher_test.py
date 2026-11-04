#!/usr/bin/env python3
"""Exercise the actual launcher with a synthetic DRM inventory and executable."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GpuSelectionLauncherTest(unittest.TestCase):
    def setUp(self):
        (ROOT / "build").mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="gpu-launcher-", dir=ROOT / "build")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        scripts = self.root / "scripts"
        scripts.mkdir()
        shutil.copy2(ROOT / "scripts/build_paths.sh", scripts / "build_paths.sh")
        inventory = self.root / "drm"
        inventory.mkdir()
        self.inventory = inventory
        for index, bdf, vendor, device, vram in [
            (0, "0000:03:00.0", "0x8086", "0xe223", None),
            (1, "0000:09:00.0", "0x1002", "0x164e", 512 * 1024 * 1024),
            (2, "0000:0a:00.0", "0x1002", "0x164e", 512 * 1024 * 1024),
        ]:
            pci = self.root / bdf
            pci.mkdir()
            for name, value in {"vendor": vendor, "device": device, "class": "0x030000"}.items():
                (pci / name).write_text(value + "\n")
            if vram is not None:
                (pci / "mem_info_vram_total").write_text(str(vram) + "\n")
            card = inventory / f"card{index}"
            card.mkdir()
            (card / "device").symlink_to(pci)
        # Replace only the OS inventory root in a disposable copy; all parsing,
        # selection, environment and exec behavior comes from the real script.
        source = (ROOT / "scripts/run.sh").read_text()
        self.assertEqual(source.count("/sys/class/drm/card*/device"), 1)
        self.launcher = scripts / "run.sh"
        self.launcher.write_text(source.replace("/sys/class/drm/card*/device", f"{inventory}/card*/device"))
        self.launcher.chmod(0o755)
        (self.root / "assets").mkdir()
        (self.root / "config.json").write_text("{}\n")
        probe = self.root / "probe.py"
        probe.write_text(
            "#!/usr/bin/env python3\nimport json,os,sys\n"
            "names=['MESA_VK_DEVICE_SELECT','MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE',"
            "'DRI_PRIME','HEAVEN_EXPECT_GPU_ID','HEAVEN_EXPECT_GPU_PCI','SDL_VIDEODRIVER']\n"
            "print('PROBE=' + json.dumps({'environment':{k:os.environ[k] for k in names if k in os.environ},"
            "'argv':sys.argv[1:]}))\n"
        )
        probe.chmod(0o755)
        self.environment = dict(os.environ)
        for name in ["MESA_VK_DEVICE_SELECT", "MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE",
                     "DRI_PRIME", "HEAVEN_EXPECT_GPU_ID", "HEAVEN_EXPECT_GPU_PCI"]:
            self.environment.pop(name, None)
        self.environment.update(HEAVEN_BIN=str(probe), HEAVEN_DATA=str(self.root / "assets"),
                                HEAVEN_CONFIG=str(self.root / "config.json"))

    def launch(self, *args, environment=None):
        env = dict(self.environment)
        env.update(environment or {})
        completed = subprocess.run([str(self.launcher), *args], env=env, text=True,
                                   capture_output=True, check=False, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        line = next(line for line in completed.stdout.splitlines() if line.startswith("PROBE="))
        return json.loads(line.removeprefix("PROBE=")), completed.stdout

    def test_explicit_id_isolates_requested_device(self):
        probe, output = self.launch("--gpu", "1002:164e")
        env = probe["environment"]
        self.assertEqual(env.get("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE"), "1")
        self.assertEqual(env.get("MESA_VK_DEVICE_SELECT"), "1002:164e")
        self.assertEqual(env.get("DRI_PRIME"), "pci-0000_09_00_0")
        self.assertEqual(env.get("HEAVEN_EXPECT_GPU_ID"), "1002:164e")
        self.assertNotIn("HEAVEN_EXPECT_GPU_PCI", env)
        self.assertIn("Requested GPU", output)

    def test_index_and_pci_preserve_exact_adapter_identity(self):
        for selector in ["2", "0000:0a:00.0"]:
            with self.subTest(selector=selector):
                probe, _ = self.launch("--gpu", selector)
                env = probe["environment"]
                self.assertEqual(env.get("HEAVEN_EXPECT_GPU_ID"), "1002:164e")
                self.assertEqual(env.get("HEAVEN_EXPECT_GPU_PCI"), "0000:0a:00.0")
                self.assertEqual(env.get("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE"), "1")

    def test_environment_preferences_remain_unchanged(self):
        overrides = {"MESA_VK_DEVICE_SELECT": "8086:e223", "DRI_PRIME": "pci-0000_03_00_0",
                     "MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE": "1"}
        probe, output = self.launch(environment=overrides)
        for name, value in overrides.items():
            self.assertEqual(probe["environment"].get(name), value)
        self.assertNotIn("HEAVEN_EXPECT_GPU_ID", probe["environment"])
        self.assertIn("Keeping GPU selection from environment", output)

    def test_explicit_default_clears_selection_and_private_checks(self):
        overrides = {"MESA_VK_DEVICE_SELECT": "1002:164e", "DRI_PRIME": "1",
                     "MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE": "1",
                     "HEAVEN_EXPECT_GPU_ID": "1002:164e", "HEAVEN_EXPECT_GPU_PCI": "0000:09:00.0"}
        probe, _ = self.launch("--gpu", "default", environment=overrides)
        for name in overrides:
            self.assertNotIn(name, probe["environment"])

    def test_policy_labels_remain_unenforced_preferences(self):
        for policy in ["auto", "integrated", "dedicated", "discrete"]:
            with self.subTest(policy=policy):
                probe, output = self.launch("--gpu", policy)
                self.assertNotIn("HEAVEN_EXPECT_GPU_ID", probe["environment"])
                self.assertNotIn("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", probe["environment"])
                self.assertIn("GPU preference", output)

    def test_opengl_dri_prime_remains_plain_pci_selector(self):
        probe, _ = self.launch("--x11", "--gpu", "0")
        self.assertEqual(probe["environment"].get("DRI_PRIME"), "pci-0000_03_00_0")
        self.assertEqual(probe["environment"].get("SDL_VIDEODRIVER"), "x11")

    def test_concrete_override_replaces_stale_expected_pci(self):
        probe, _ = self.launch("--gpu", "8086:e223", environment={
            "MESA_VK_DEVICE_SELECT": "1002:164e", "DRI_PRIME": "pci-0000_09_00_0",
            "HEAVEN_EXPECT_GPU_ID": "1002:164e", "HEAVEN_EXPECT_GPU_PCI": "0000:09:00.0"})
        self.assertEqual(probe["environment"].get("HEAVEN_EXPECT_GPU_ID"), "8086:e223")
        self.assertNotIn("HEAVEN_EXPECT_GPU_PCI", probe["environment"])
        self.assertEqual(probe["environment"].get("MESA_VK_DEVICE_SELECT"), "8086:e223")

    def test_missing_drm_checks_id_and_refuses_unresolvable_index(self):
        for card in self.inventory.iterdir():
            (card / "device").unlink()
            card.rmdir()
        probe, _ = self.launch("--gpu", "1002:164e")
        self.assertEqual(probe["environment"].get("HEAVEN_EXPECT_GPU_ID"), "1002:164e")
        self.assertEqual(probe["environment"].get("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE"), "1")
        failed = subprocess.run([str(self.launcher), "--gpu", "0"], env=self.environment,
                                capture_output=True, text=True, check=False, timeout=10)
        self.assertEqual(failed.returncode, 2)
        self.assertIn("cannot resolve GPU selector", failed.stderr)


if __name__ == "__main__":
    unittest.main()
