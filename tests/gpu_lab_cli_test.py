"""GPU-independent native CLI checks; no Vulkan context is created."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--executable', type=Path, required=True)
arguments, remaining = parser.parse_known_args()
executable = arguments.executable.resolve(strict=True)


class CliTest(unittest.TestCase):
    def invoke(self, *options):
        return subprocess.run([str(executable), *options], text=True, capture_output=True, timeout=10)

    def test_help_without_device(self):
        result = self.invoke('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--gpu', result.stdout)
        self.assertIn('No scene FPS', result.stdout)
        self.assertIn('--workload fp16|int8', result.stdout)
        self.assertIn('dp4a', result.stdout)

    def test_missing_or_invalid_options_fail_before_gpu_initialization(self):
        for options in [(), ('--gpu',), ('--gpu', '8086'), ('--gpu', '8086:10000'),
                        ('--gpu', '0:e223'), ('--gpu', '8086:e223garbage'),
                        ('--seconds', '59'), ('--seconds', '601'), ('--seconds', '-1'),
                        ('--seconds', 'NaN'), ('--kernel', 'xess'), ('--unknown', 'x'),
                        ('--workload', 'uint8'), ('--gpu', '8086:e223', '--kernel', 'dp4a'),
                        ('--gpu', '8086:e223', '--negative-control', 'zero-store'),
                        ('--gpu', '8086:e223', '--workload', 'int8', '--kernel', 'reference', '--negative-control', 'zero-store'),
                        ('--gpu', '8086:e223', '--workload', 'int8', '--quick-check', '--negative-control', 'unknown')]:
            with self.subTest(options=options):
                result = self.invoke(*options)
                self.assertEqual(result.returncode, 1)
                self.assertIn('GPU lab:', result.stderr)
                self.assertNotIn('Create Vulkan', result.stderr)

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory(prefix='gpu-lab-cli-') as temporary:
            path = Path(temporary) / 'capture.json'
            path.write_text('retained previous capture')
            result = self.invoke('--gpu', '8086:e223', '--output', str(path))
            self.assertEqual(result.returncode, 1)
            self.assertIn('Output already exists', result.stderr)
            self.assertEqual(path.read_text(), 'retained previous capture')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])
