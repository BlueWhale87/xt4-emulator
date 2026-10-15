"""Independent original-bootstrap oracle, with declared synthetic inputs/time."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from xt4lab.engine import source_fingerprint
from xt4lab.firmware import prepare_run
from xt4lab.report import write_report
from xt4lab.run import RunSession
from xt4lab.startup import extract_bootstrap

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
PROFILE = 'bootstrap-timer-exploration'
COUNTS = {
    'xt4-port-config-v1': {'writes': 97, 'reads': 0, 'transitions': 0},
    'xt4-static-port-input-v1': {'writes': 0, 'reads': 16, 'transitions': 0},
    'xt4-timer-exploration-v1': {'writes': 10, 'reads': 1, 'transitions': 0},
}


class TimerRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=REPO / 'runs')
        cls.root = Path(cls.tmp.name)
        original = (RESEARCH / 'FWUP0024.DAT').read_bytes()
        cls.original_hash = hashlib.sha256(original).hexdigest()
        cls.bootstrap = extract_bootstrap(original)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_original(self, name, scenario='zero-ports-v1'):
        path = self.root / name
        if not path.exists():
            run = prepare_run(RESEARCH, path, PROFILE, scenario_name=scenario)
            s = RunSession(run, REPO / 'build/qemu-system-arm', budget=30000)
            try:
                s.continue_steps(30000)
                self.assertEqual(struct.unpack('<16I', s.client.read_memory(0x10aec, 64)),
                    (0 if scenario == 'zero-ports-v1' else 0xffffffff,) * 16)
            finally:
                s.close()
            write_report(path)
        return json.loads((path / 'summary.json').read_text())

    def test_original_oracle_both_scenarios(self):
        for name, word in [('zero-ports-v1', 0), ('one-ports-v1', 0xffffffff)]:
            with self.subTest(scenario=name):
                data = self.run_original(name, name)
                stop = data['stop']
                self.assertEqual((stop['reason'], stop['pc'], stop['physical_address'],
                                  stop['access'], stop['width'], stop['value']),
                    ('unsupported_access', 0x4f7c, 0xfffe0000, 'read', 4, None))
                self.assertNotIn('device', stop)
                self.assertNotIn('detail', stop)
                self.assertGreater(data['completed_steps'], 19613)
                self.assertLess(data['completed_steps'], 30000)
                self.assertEqual((data['modeled_write_count'], data['modeled_read_count']), (107, 17))
                self.assertTrue('modeled_operations_by_model' in data, 'missing per-model operation counts')
                self.assertEqual(data['modeled_operations_by_model'], COUNTS)
                self.assertFalse(data['camera_booted'])
                events = json.loads((self.root / name / 'device-events.json').read_text())
                port_writes = [e for e in events if e['device'] == 'xt4-port-config-v1']
                want = []
                # Independent packaged initialization table, not model logic.
                for port in range(16):
                    row = struct.unpack_from('<7I', self.bootstrap, 0xa188 + port * 28)
                    for selector, field in [(6, 0), (3, 1), (4, 2), (2, 3), (0, 5), (1, 4)]:
                        want.append((0xfff41000 + port * 0x40 + selector * 4, row[field]))
                want.append((0xfff4109c, struct.unpack_from('<I', self.bootstrap, 0xa094)[0]))
                self.assertEqual([(e['address'], e['value']) for e in port_writes], want)
                inputs = [e for e in events if e['device'] == 'xt4-static-port-input-v1']
                self.assertEqual([(e['address'], e['value']) for e in inputs],
                                 [(0xfff41014 + port * 0x40, word) for port in range(16)])
                timer = [e for e in events if e['device'] == 'xt4-timer-exploration-v1']
                self.assertEqual([(e['address'], e['value']) for e in timer if e['operation'] == 'write'],
                    [(0xfff3c348, 540000)] * 3 + [(0xfff3c34c, 539999)] * 3 +
                    [(0xfff3c348, 0xffffffff)] * 3 + [(0xfff3c300, 0x00800080)])
                reads = [e for e in timer if e['operation'] == 'read']
                self.assertEqual(len(reads), 1)
                self.assertEqual(reads[0]['address'], 0xfff3c348)
                elapsed = reads[0]['virtual_ns'] - timer[-2]['virtual_ns']
                self.assertEqual(reads[0]['value'], 0xffffffff - elapsed * 54000000 // 1000000000)
                self.assertTrue(all(e['success'] for e in events))

    def test_repeatability_and_original_bytes(self):
        a = self.run_original('zero-ports-v1')
        b = self.run_original('repeat')
        other = self.run_original('one-ports-v1', 'one-ports-v1')
        for key in ['canonical_trace_sha256', 'canonical_device_events_sha256', 'scenario_sha256']:
            self.assertEqual(a[key], b[key])
        self.assertEqual(a['completed_pcs'], other['completed_pcs'])
        self.assertEqual(a['stop'], other['stop'])
        self.assertNotEqual(a['canonical_device_events_sha256'], other['canonical_device_events_sha256'])
        manifest = json.loads((REPO / 'build/xt4-build.json').read_text())
        self.assertEqual(manifest['source_files_sha256'], source_fingerprint(REPO))
        self.assertEqual(manifest['binary_sha256'], hashlib.sha256(
            (REPO / 'build/qemu-system-arm').read_bytes()).hexdigest())
        self.assertEqual(hashlib.sha256((RESEARCH / 'FWUP0024.DAT').read_bytes()).hexdigest(),
                         self.original_hash)
        for name in ['zero-ports-v1', 'repeat', 'one-ports-v1']:
            path = self.root / name
            self.assertEqual((path / 'inputs/bootstrap.bin').read_bytes(), self.bootstrap)
            self.assertEqual((path / 'segments/000/inputs/bootstrap.bin').read_bytes(), self.bootstrap)
            trace = [json.loads(line) for line in (path / 'trace.jsonl').read_text().splitlines()]
            instructions = [e for e in trace if e['event'] in ('attempt', 'instruction')]
            self.assertTrue(instructions)
            for event in instructions:
                pc = event['pc']
                self.assertEqual(event['bytes'], self.bootstrap[pc:pc + 4].hex())

    def test_reset_retains_evidence_counts_latest_segment(self):
        run = prepare_run(RESEARCH, self.root / 'reset', PROFILE, scenario_name='zero-ports-v1')
        s = RunSession(run, REPO / 'build/qemu-system-arm', budget=30000)
        try:
            s.continue_steps(30000)
            write_report(run.directory)
            before = json.loads((run.directory / 'summary.json').read_text())
            old = (run.directory / 'segments/000/engine-events.jsonl').read_bytes()
            s.reset()
            s.step()
            write_report(run.directory)
            partial = json.loads((run.directory / 'summary.json').read_text())
            self.assertEqual(partial['modeled_operations_by_model'], {
                m: dict(writes=0, reads=0, transitions=0) for m in COUNTS})
            self.assertEqual(json.loads((run.directory / 'device-events.json').read_text()), [])
            s.continue_steps(29999)
        finally:
            s.close()
        write_report(run.directory)
        after = json.loads((run.directory / 'summary.json').read_text())
        self.assertEqual(after['stop']['segment'], 1)
        self.assertEqual(after['modeled_operations_by_model'], COUNTS)
        self.assertEqual(after['canonical_device_events_sha256'], before['canonical_device_events_sha256'])
        self.assertEqual(after['scenario_sha256'], before['scenario_sha256'])
        self.assertEqual((run.directory / 'segments/000/engine-events.jsonl').read_bytes(), old)
        self.assertTrue(all(e['evidence'].startswith('segments/001/') for e in
                            json.loads((run.directory / 'device-events.json').read_text())))
