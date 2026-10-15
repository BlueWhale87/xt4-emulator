from collections import Counter
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.report import write_report
from xt4lab.run import RunSession

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'


class BootstrapRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=REPO / 'runs')
        cls.paths = [Path(cls.tmp.name) / name for name in ['a', 'b']]
        cls.original_hash = hashlib.sha256((RESEARCH / 'FWUP0024.DAT').read_bytes()).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_original(self, path):
        if not path.exists():
            prepared = prepare_run(RESEARCH, path, 'bootstrap-entry-diagnostic')
            session = RunSession(prepared, REPO / 'build/qemu-system-arm', budget=30000)
            try:
                session.continue_steps(30000)
            finally:
                session.close()
            write_report(path)
        return json.loads((path / 'summary.json').read_text())

    def test_bootstrap_first_hardware_write(self):
        data = self.run_original(self.paths[0])
        self.assertEqual(data['completed_pcs'][:3], [0, 0x100, 0x184])
        stop = data['stop']
        self.assertEqual(stop['reason'], 'unsupported_access', stop)
        self.assertEqual(stop['pc'], 0x4f34)
        self.assertEqual(stop['physical_address'], 0xfff41018)
        self.assertEqual((stop['access'], stop['width'], stop['value']),
                         ('write', 4, 0xc0ff3f0f))
        counts = Counter(data['completed_pcs'])
        # Original ARM loops: 4624 BSS stores plus a final false-condition
        # instruction, and 224 eight-register stack-clear stores.
        self.assertEqual(counts[0x144], 4625)
        self.assertEqual(counts[0x26c], 224)
        self.assertFalse(data['camera_booted'])
        self.assertEqual(data['profile'], 'bootstrap-entry-diagnostic')

    def test_bootstrap_report_describes_actual_boundary(self):
        self.run_original(self.paths[0])
        text = (self.paths[0] / 'report.md').read_text()
        self.assertIn('bootstrap-entry-diagnostic', text)
        self.assertNotIn('This is a module-entry diagnostic', text)
        self.assertNotIn('synthetic stack', text.lower())

    def test_repeatable_bootstrap_and_immutable_input(self):
        a = self.run_original(self.paths[0])
        b = self.run_original(self.paths[1])
        self.assertEqual(a['stop'], b['stop'])
        self.assertEqual(a['canonical_trace_sha256'], b['canonical_trace_sha256'])
        self.assertEqual(a['canonical_trace'], b['canonical_trace'])
        self.assertEqual(hashlib.sha256((RESEARCH / 'FWUP0024.DAT').read_bytes()).hexdigest(),
                         self.original_hash)


if __name__ == '__main__':
    unittest.main()
