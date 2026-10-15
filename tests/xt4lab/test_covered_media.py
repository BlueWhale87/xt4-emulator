"""Exact covered calibration extraction, without publishing source bytes."""
import hashlib
from pathlib import Path
import unittest
from xt4lab import startup

REPO=Path(__file__).resolve().parents[2]
RESEARCH=REPO.parents[1]/'research/2026-10-14-xt4-firmware'


class CoveredMediaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original=(RESEARCH/'FWUP0024.DAT').read_bytes()

    def test_exact_full_covered_extent(self):
        data=startup.extract_calibration(self.original)
        self.assertEqual(len(data),128)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         '10fa7918107458a1b72e18ef91b9bfc27b6eb76e3b562aa6566c5561dcf4929a')
        self.assertEqual(data,bytes(v^0xff for v in self.original[0x2a2f4:0x2a374]))

    def test_coverage_agrees_with_original_bootstrap(self):
        self.assertEqual(startup.extract_calibration(self.original),
                         startup.extract_bootstrap(self.original)[0xa080:0xa100])

    def test_wrong_whole_input_rejected(self):
        for data in (self.original[:-1],b'X'+self.original[1:],
                     self.original[:0x2a2f4]+bytes([self.original[0x2a2f4]^1])+self.original[0x2a2f5:]):
            with self.assertRaises(ValueError):startup.extract_calibration(data)


if __name__=='__main__':unittest.main()
