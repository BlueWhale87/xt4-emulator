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
# Original single-step device-event oracles, retained when using compact runs.
DEVICE_HASHES = {
    'zero-ports-v1': '69795b5f0a9730e2a7fe4a60977380436880eb3f1f3c3fd1838569167ced6ea8',
    'one-ports-v1': 'b6a3f63730083d21e79c3188ce2e9e82fcc8dcfffc864bce8f6ade70d3f2a1a2',
}


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
                    s = RunSession(run, REPO / 'build/qemu-system-arm', budget=30000,
                                   execution_mode='bounded-continue')
                    try:
                        s.continue_fast()
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
                    self.assertGreater(summary['engine_instruction_count'], 19613)
                    self.assertEqual(summary['completed_steps'], 0)
                    self.assertEqual(summary['completed_pcs'], [])
                    self.assertEqual(summary['canonical_device_events_sha256'], DEVICE_HASHES[name])
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
