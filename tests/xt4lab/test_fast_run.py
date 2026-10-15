"""Fast execution must retain strict stops without inventing a CPU trace."""
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.report import write_report
from xt4lab.run import RunSession

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
QEMU = REPO / 'build/qemu-system-arm'


class FastRunTests(unittest.TestCase):
    def prepared(self, name='run', profile='module-entry-diagnostic', scenario=None):
        tmp = tempfile.TemporaryDirectory(dir=REPO / 'runs')
        self.addCleanup(tmp.cleanup)
        return prepare_run(RESEARCH, Path(tmp.name) / name, profile, scenario_name=scenario)

    def session(self, **kwargs):
        self.assertIn('execution_mode', __import__('inspect').signature(RunSession).parameters,
                      'explicit execution mode missing')
        run = self.prepared()
        s = RunSession(run, QEMU, execution_mode='bounded-continue', **kwargs)
        self.addCleanup(s.close)
        return s

    def test_exact_budget_launch_and_compact_report(self):
        s = self.session(budget=1000003)
        self.assertIn('instruction-budget=1000003', s.argv[2])
        self.assertIn('-icount', s.argv)
        self.assertFalse(any('tcp:' in a or 'usb-host' in a for a in s.argv))
        self.assertEqual(s.socket_path.stat().st_mode & 0o777, 0o600)
        # Test-only add/store/branch loop in anonymous RAM, not input bytes.
        base = 0x12910c4
        raw = struct.pack('<3I', 0xe2800001, 0xe5820000, 0xeafffffc)
        self.assertEqual(s.client.command(f'M{base:x},{len(raw):x}:' + raw.hex()), 'OK')
        # Set r0=0 and r2 to scratch RAM through discovered ARM registers.
        self.assertEqual(s.client.command('P0=00000000'), 'OK')
        self.assertEqual(s.client.command('P2=00000002'), 'OK')
        result = s.continue_fast()
        self.assertEqual(result['reason'], 'instruction_budget')
        self.assertEqual(result['engine_instruction_count'], 1000003)
        self.assertEqual(result['completed_steps'], 0)
        self.assertEqual(s.client.registers()['pc'], base + 4)
        self.assertEqual(int.from_bytes(s.client.read_memory(0x2000000, 4), 'little'), 333334)
        s.close(); write_report(s.prepared.directory)
        summary = json.loads((s.prepared.directory / 'summary.json').read_text())
        self.assertEqual(summary['execution_mode'], 'bounded-continue')
        self.assertEqual(summary['engine_instruction_count'], 1000003)
        self.assertEqual(summary['completed_pcs'], [])
        self.assertEqual(summary['completed_steps'], 0)
        self.assertEqual(summary['trace_policy'], 'boundary-and-device-events')
        self.assertFalse(summary['camera_booted'])
        self.assertLess(len(summary['canonical_trace']), 10)

    def test_admission_rejects_bad_modes_types_and_mixed_steps(self):
        self.assertIn('execution_mode', __import__('inspect').signature(RunSession).parameters,
                      'explicit execution mode missing')
        for kwargs in [dict(execution_mode='fast'), dict(execution_mode=True),
                       *[dict(execution_mode='bounded-continue', budget=b)
                         for b in [True, 0, -1, 1.5, 1000000001]],
                       dict(execution_mode='single-step', budget=1000001)]:
            with self.subTest(kwargs=kwargs):
                run = self.prepared()
                with self.assertRaises(ValueError):
                    RunSession(run, QEMU, **kwargs)
                self.assertFalse((run.directory / 'trace.jsonl').exists())
        s = self.session(budget=1)
        for call in [s.step, lambda: s.continue_steps(1)]:
            with self.assertRaisesRegex(ValueError, 'mode|single-step'):
                call()
        run = self.prepared()
        legacy = RunSession(run, QEMU, budget=1)
        self.addCleanup(legacy.close)
        with self.assertRaisesRegex(ValueError, 'mode|bounded'):
            legacy.continue_fast()

    def test_pause_resume_cumulative_budget_and_reset(self):
        s = self.session(budget=100000000)
        self.assertEqual(s.client.command('M12910c4,4:feffffea'), 'OK')
        timer = threading.Timer(.01, lambda: os.kill(os.getpid(), signal.SIGINT))
        timer.start()
        try:
            result = s.continue_fast()
        finally:
            timer.join(1)
        self.assertEqual(result['reason'], 'paused')
        self.assertGreater(result['engine_instruction_count'], 0)
        self.assertLess(result['engine_instruction_count'], 100000000)
        self.assertIn('pc', s.client.registers())
        self.assertEqual(s.continue_fast()['engine_instruction_count'], 100000000)
        old = (s.segment_dir / 'engine-events.jsonl').read_bytes()
        s.reset()
        self.assertEqual(s.engine_instruction_count, 0)
        self.assertGreater(s.continue_fast()['engine_instruction_count'], 0)
        self.assertEqual(s.stop['reason'], 'unsupported_access')
        self.assertEqual((s.prepared.directory / 'segments/000/engine-events.jsonl').read_bytes(), old)
        write_report(s.prepared.directory)
        data = json.loads((s.prepared.directory / 'summary.json').read_text())
        self.assertEqual(data['engine_instruction_count'], s.engine_instruction_count)
        self.assertEqual(data['stop']['segment'], 1)

    def test_wfi_timeout_reaps_child(self):
        s = self.session(budget=100, timeout=.1, wall_limit=.04)
        self.assertEqual(s.client.command('M12910c4,4:03f020e3'), 'OK')
        process = s.process
        self.assertEqual(s.continue_fast()['reason'], 'timeout')
        self.assertIsNotNone(process.poll())

    def test_crash_disconnect_and_missing_fresh_accounting_are_unknown(self):
        for failure in ['crash', 'disconnect', 'missing']:
            with self.subTest(failure=failure):
                s = self.session(budget=1)
                process = s.process
                if failure == 'crash':
                    process.kill(); process.wait()
                elif failure == 'disconnect':
                    s.client.close()
                else:
                    # Corrupt the evidence reader only; the real engine still runs.
                    s._execution_snapshot = lambda: None
                result = s.continue_fast()
                self.assertIn(result['reason'], ['engine_crash', 'debugger_disconnected',
                                                'debugger_protocol_error'])
                self.assertIsNone(result['engine_instruction_count'])
                self.assertIsNotNone(process.poll())
                write_report(s.prepared.directory)
                self.assertIsNone(json.loads((s.prepared.directory / 'summary.json').read_text())[
                    'engine_instruction_count'])

    def test_cli_fast_interactive_rejected_before_staging(self):
        run = self.prepared()
        path = run.directory / 'should-not-exist'
        result = subprocess.run([sys.executable, '-m', 'xt4lab', 'run', '--research-root',
            str(RESEARCH), '--out', str(path), '--fast', '--interactive'],
            cwd=REPO, text=True, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('interactive', result.stderr.lower())
        self.assertFalse(path.exists())

    def test_fast_identity_cannot_be_removed_or_forged(self):
        s = self.session(budget=1)
        s.continue_fast(); s.close()
        path = s.prepared.directory / 'run-info.json'
        original = path.read_text()
        for mutation in ['remove', 'mode', 'budget', 'boolean', 'trace', 'semantics']:
            with self.subTest(mutation=mutation):
                info = json.loads(original)
                if mutation == 'remove':
                    for key in ['execution_mode', 'trace_policy', 'count_semantics']:
                        info.pop(key)
                elif mutation == 'mode': info['execution_mode'] = 'single-step'
                elif mutation == 'budget': info['budget'] = 2
                elif mutation == 'boolean': info['budget'] = True
                elif mutation == 'trace': info['trace_policy'] = 'instruction'
                else: info['count_semantics'] = 'retired'
                path.write_text(json.dumps(info))
                with self.assertRaises(ValueError): write_report(s.prepared.directory)

    def test_failed_resume_cannot_reuse_prior_pause_count(self):
        s = self.session(budget=100000000)
        self.assertEqual(s.client.command('M12910c4,4:feffffea'), 'OK')
        timer = threading.Timer(.01, lambda: os.kill(os.getpid(), signal.SIGINT))
        timer.start()
        try:
            paused = s.continue_fast()
        finally:
            timer.join(1)
        self.assertEqual(paused['reason'], 'paused')
        self.assertGreater(paused['engine_instruction_count'], 0)
        s._execution_snapshot = lambda: None
        failed = s.continue_fast()
        self.assertEqual(failed['reason'], 'debugger_protocol_error')
        self.assertIsNone(failed['engine_instruction_count'])
        self.assertIsNotNone(s.process.poll())
        write_report(s.prepared.directory)
        data = json.loads((s.prepared.directory / 'summary.json').read_text())
        self.assertIsNone(data['engine_instruction_count'])

    def test_original_bootstrap_matches_single_step_oracle(self):
        hashes = {'zero-ports-v1': '4b59161adeb67c9efcc26b5d057f4accda8154593ff01b025224fcbe61204d66',
                  'one-ports-v1': 'cedb34869d1eac668d10fb6badf78be6934053f218594e33e3cd8d0897fbfdb6'}
        summaries = []
        for scenario in ['zero-ports-v1', 'one-ports-v1', 'zero-ports-v1']:
            with self.subTest(scenario=scenario):
                run = self.prepared(profile='bootstrap-timer-exploration', scenario=scenario)
                original = (run.directory / 'inputs/bootstrap.bin').read_bytes()
                s = RunSession(run, QEMU, budget=30000, execution_mode='bounded-continue')
                try:
                    result = s.continue_fast()
                    self.assertEqual((result['reason'], result['pc'], result['physical_address'],
                                      result['access'], result['width'], result['value']),
                        ('unsupported_access', 0x4f7c, 0xfffe0000, 'read', 4, None))
                    self.assertGreater(result['engine_instruction_count'], 20600)
                    self.assertEqual(s.client.read_memory(0x10aec, 64),
                        (b'\0' if scenario == 'zero-ports-v1' else b'\xff') * 64)
                finally:
                    s.close()
                write_report(run.directory)
                data = json.loads((run.directory / 'summary.json').read_text())
                self.assertEqual(data['canonical_device_events_sha256'], hashes[scenario])
                self.assertEqual((data['modeled_write_count'], data['modeled_read_count']), (107, 17))
                self.assertEqual(data['completed_steps'], 0)
                self.assertEqual(data['completed_pcs'], [])
                self.assertEqual((run.directory / 'inputs/bootstrap.bin').read_bytes(), original)
                self.assertEqual((run.directory / 'segments/000/inputs/bootstrap.bin').read_bytes(), original)
                summaries.append(data)
        self.assertEqual(summaries[0]['canonical_trace_sha256'], summaries[2]['canonical_trace_sha256'])
        self.assertEqual(summaries[0]['engine_instruction_count'], summaries[1]['engine_instruction_count'])


if __name__ == '__main__':
    unittest.main()
