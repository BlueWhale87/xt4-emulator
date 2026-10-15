"""Original input snapshot and first timer access, never a camera connection."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.report import write_report
from xt4lab.run import RunSession

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'


class InputRunTests(unittest.TestCase):
    def test_scenarios_reach_first_timer_write(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            for name, value in [('zero-ports-v1', 0), ('one-ports-v1', 0xffffffff)]:
                with self.subTest(name=name):
                    try:
                        run = prepare_run(RESEARCH, Path(tmp) / name,
                            'bootstrap-input-exploration', scenario_name=name)
                    except TypeError as exc:
                        self.fail('Input scenario execution is missing: ' + str(exc))
                    s = RunSession(run, REPO / 'build/qemu-system-arm', budget=30000)
                    try:
                        s.continue_steps(30000)
                        stop = s.stop
                        self.assertEqual((stop['reason'], stop.get('pc'), stop.get('physical_address'),
                                          stop.get('width'), stop.get('value')),
                            ('unsupported_access', 0x4f34, 0xfff3c348, 4, 540000))
                        cache = s.client.read_memory(0x10aec, 64)
                        self.assertEqual(struct.unpack('<16I', cache), (value,) * 16)
                    finally:
                        s.close()
                    write_report(run.directory)
                    summary = json.loads((run.directory / 'summary.json').read_text())
                    self.assertGreater(summary['completed_steps'], 19613)
                    self.assertEqual((summary['modeled_write_count'], summary['modeled_read_count']), (97, 16))
                    self.assertEqual(summary['execution_class'], 'synthetic_exploration')
                    self.assertEqual(summary['scenario']['scenario_id'], name)
                    self.assertTrue(summary['scenario_sha256'])
                    records = [json.loads(line) for line in
                        (run.directory/'segments/000/engine-events.jsonl').read_text().splitlines()]
                    reset = next(r for r in records if r['event'] == 'reset')
                    self.assertEqual(reset['scenario_sha256'], summary['scenario_sha256'])
                    self.assertFalse(summary['camera_booted'])
                    events = json.loads((run.directory / 'device-events.json').read_text())
                    reads = [e for e in events if e['operation'] == 'read']
                    self.assertEqual([(e['address'], e['value']) for e in reads],
                                     [(0xfff41014 + port * 0x40, value) for port in range(16)])
                    self.assertTrue(all(e['device'] == 'xt4-static-port-input-v1' for e in reads))
                    self.assertTrue(all('synthetic' in e['semantics'] for e in reads))
