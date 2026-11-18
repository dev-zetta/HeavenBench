"""CPU coverage of observer continuity; never opens a GPU."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from profile_harness.power_observer import validate_csv


def samples(start=0):
    rows=[]
    for i in range(160):
        t=(start+i*2)%86400
        rows.append(f'[{t//3600:02d}:{t//60%60:02d}:{t%60:02d}.000] 180.0, 2800, 550, None, 9800.0')
    return '\n'.join(rows)


class Coverage(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(validate_csv(samples(),300)['rows'],160)

    def test_midnight(self):
        self.assertEqual(validate_csv(samples(86300),300)['span_seconds'],318)

    def test_truncated(self):
        with self.assertRaises(ValueError):validate_csv('\n'.join(samples().splitlines()[:110]),300)

    def test_gap(self):
        with self.assertRaises(ValueError):validate_csv('\n'.join(samples().splitlines()[:20]+samples().splitlines()[24:]),300)

    def test_unavailable(self):
        with self.assertRaises(ValueError):validate_csv(samples().replace('180.0','N/A'),300)

    def test_replay(self):
        with self.assertRaises(ValueError):validate_csv('\n'.join([samples().splitlines()[0]]*160),300)

    def test_nonfinite(self):
        with self.assertRaises(ValueError):validate_csv(samples().replace('180.0','nan'),300)

    def test_bound(self):
        with self.assertRaises(ValueError):validate_csv(samples(),float('nan'))


if __name__=='__main__':unittest.main()
