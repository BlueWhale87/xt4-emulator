import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.run import RunSession

REPO=Path(__file__).resolve().parents[2]
RESEARCH=REPO.parents[1]/'research/2026-10-14-xt4-firmware'


class FirmwareRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(dir=REPO/'runs')
        cls.paths=[Path(cls.tmp.name)/n for n in ['a','b']]
        cls.pins={p:hashlib.sha256((RESEARCH/p).read_bytes()).hexdigest()
            for p in ['FWUP0024.DAT','unpacked/segment-260274.bin',
                      'unpacked/segment-5c0274.bin','unpacked/segment-ae0274.bin']}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_original(self,path):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.report'),'evidence reporting missing')
        from xt4lab.report import write_report
        if not path.exists():
            prepared=prepare_run(RESEARCH,path,'module-entry-diagnostic')
            session=RunSession(prepared,REPO/'build/qemu-system-arm')
            try:
                session.continue_steps(1000)
            finally:
                session.close()
            write_report(path)
        return json.loads((path/'summary.json').read_text())

    def test_original_entry_to_missing_handoff(self):
        data=self.run_original(self.paths[0])
        self.assertEqual(data['completed_pcs'],[0x012910c4,0x012910c8,0x012910cc,
            0x012910d0,0x01291014,0x01291018,0x0129101c,0x01291020])
        self.assertEqual(data['completed_steps'],8)
        self.assertEqual(data['stop']['attempted_steps'],9)
        self.assertEqual(data['stop']['reason'],'unsupported_access')
        self.assertEqual(data['stop']['pc'],0x01291024)
        self.assertEqual(data['stop']['physical_address'],0x00e20008)
        self.assertEqual(data['stop']['access'],'read')
        self.assertEqual(data['stop']['width'],1)
        self.assertFalse(data['camera_booted'])
        self.assertGreaterEqual(len(data['assumptions']),6)

    def test_repeatable_diagnostic(self):
        a=self.run_original(self.paths[0]);b=self.run_original(self.paths[1])
        self.assertEqual(a['canonical_trace'],b['canonical_trace'])
        self.assertEqual(a['canonical_trace_sha256'],b['canonical_trace_sha256'])

    def test_input_immutability(self):
        self.run_original(self.paths[0]);self.run_original(self.paths[1])
        for p,sha in self.pins.items():
            self.assertEqual(hashlib.sha256((RESEARCH/p).read_bytes()).hexdigest(),sha)

if __name__=='__main__':
    unittest.main()
