import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.report import write_report
from xt4lab.run import RunSession
from xt4lab.startup import extract_bootstrap

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
PROFILE = 'bootstrap-port-config-diagnostic'


class PortRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=REPO / 'runs')
        cls.paths = [Path(cls.tmp.name) / x for x in ['a', 'b']]
        original = (RESEARCH / 'FWUP0024.DAT').read_bytes()
        cls.original_hash = hashlib.sha256(original).hexdigest()
        cls.bootstrap = extract_bootstrap(original)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_original(self, path):
        if not path.exists():
            run = prepare_run(RESEARCH, path, PROFILE)
            s = RunSession(run, REPO / 'build/qemu-system-arm', budget=30000,
                           execution_mode='bounded-continue')
            try:
                s.continue_fast()
            finally:
                s.close()
            write_report(path)
        return json.loads((path / 'summary.json').read_text())

    def test_original_writes_then_unknown_input(self):
        data = self.run_original(self.paths[0])
        stop = data['stop']
        self.assertEqual(stop['reason'], 'unsupported_access', stop)
        self.assertGreater(data['engine_instruction_count'], 14731)
        self.assertEqual(data['completed_steps'], 0)
        self.assertEqual(data['completed_pcs'], [])
        # Pinned from the same unchanged bootstrap in single-step mode.
        self.assertEqual(data['canonical_device_events_sha256'],
                         'aed670eda7ac3a9e46bc869ff5829b9c91e8d008d2e6081874599d657113a8dd')
        self.assertEqual((stop['pc'], stop['physical_address'], stop['width']),
                         (0x4f7c, 0xfff41014, 4))
        self.assertEqual(stop['access'], 'read')
        self.assertIsNone(stop['value'])
        self.assertEqual((stop['device'], stop['detail']),
                         ('xt4-port-config-v1', 'unknown_port_input'))
        records = [json.loads(x) for x in (self.paths[0] / 'segments/000/engine-events.jsonl').read_text().splitlines()]
        writes = [r for r in records if r.get('event') == 'device-access' and r['success'] and r['operation'] == 'write']
        self.assertEqual(len(writes), 97)
        want = []
        # Independent source-data/control-flow oracle; not the device algorithm.
        for port in range(16):
            row = struct.unpack_from('<7I', self.bootstrap, 0xa188 + port * 28)
            for selector, field in [(6,0), (3,1), (4,2), (2,3), (0,5), (1,4)]:
                want.append((0xfff41000 + port * 0x40 + selector * 4, row[field]))
        want.append((0xfff4109c, struct.unpack_from('<I', self.bootstrap, 0xa094)[0]))
        actual = [(r['address'], r['value']) for r in writes]
        self.assertEqual(actual, want)
        self.assertEqual([v for _,v in actual[:6]],
                         [0xc0ff3f0f, 0xffff1555, 0xffff0045, 0xffffffff, 0xffff1000, 0xffff9000])
        self.assertEqual(actual[-1], (0xfff4109c, 0x00015555))
        reads = [r for r in records if r.get('event') == 'device-access' and r['operation'] == 'read']
        self.assertEqual(len(reads), 1)
        self.assertFalse(reads[0]['success'])
        self.assertIsNone(reads[0]['value'])
        self.assertFalse(data['camera_booted'])

    def test_report_model_evidence(self):
        data = self.run_original(self.paths[0])
        self.assertTrue('models' in data, 'reports do not expose device models')
        self.assertEqual(data['models'], ['xt4-port-config-v1'])
        self.assertEqual((data['modeled_write_count'], data['modeled_read_count']), (97, 0))
        events = json.loads((self.paths[0] / 'device-events.json').read_text())
        self.assertEqual(len(events), 98)
        self.assertTrue(all('evidence' in e for e in events))
        self.assertIn('xt4-port-config-v1', (self.paths[0] / 'report.md').read_text())
        self.assertEqual(data['dependencies'][-1]['detail'], 'unknown_port_input')

    def test_repeatable_model_and_original_immutable(self):
        a = self.run_original(self.paths[0]); b = self.run_original(self.paths[1])
        self.assertTrue('canonical_device_events_sha256' in a, 'missing device hash')
        self.assertEqual(a['canonical_trace_sha256'], b['canonical_trace_sha256'])
        self.assertEqual(a['canonical_device_events_sha256'], b['canonical_device_events_sha256'])
        self.assertEqual(hashlib.sha256((RESEARCH / 'FWUP0024.DAT').read_bytes()).hexdigest(), self.original_hash)
        for path in self.paths:
            self.assertEqual(hashlib.sha256((path / 'inputs/bootstrap.bin').read_bytes()).hexdigest(),
                             'bf2d1d4ca9d1d60e5125a964988a3f1852e177c3176d35dc59dcc63afe41f6b1')

    def test_reset_report_counts_only_latest_segment(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp) / 'reset', PROFILE)
            s = RunSession(run, REPO / 'build/qemu-system-arm', budget=30000)
            try:
                s.continue_steps(30000)
                self.assertEqual(s.stop['detail'], 'unknown_port_input')
                s.reset()
                s.step()
            finally:
                s.close()
            write_report(run.directory)
            data = json.loads((run.directory / 'summary.json').read_text())
            self.assertTrue('modeled_write_count' in data, 'missing modeled operation counts')
            self.assertEqual(data['modeled_write_count'], 0)
            self.assertEqual(data['completed_steps'], 1)
            self.assertEqual(data['stop']['reason'], 'incomplete')
            self.assertEqual(json.loads((run.directory / 'device-events.json').read_text()), [])
            old = (run.directory / 'segments/000/engine-events.jsonl').read_text()
            self.assertIn('unknown_port_input', old)


if __name__ == '__main__':
    unittest.main()
