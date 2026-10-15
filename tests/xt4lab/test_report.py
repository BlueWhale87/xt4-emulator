import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.run import RunSession

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'


class ReportTests(unittest.TestCase):
    def test_report_truthfulness(self):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.report'), 'evidence reporting missing')
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            prepared=prepare_run(RESEARCH,Path(tmp)/'partial','module-entry-diagnostic')
            s=RunSession(prepared,REPO/'build/qemu-system-arm',budget=2)
            try:
                s.continue_steps(2)
            finally:
                s.close()
            report=write_report(prepared.directory)
            self.assertTrue(report.is_file())
            data=json.loads((prepared.directory/'summary.json').read_text())
            self.assertFalse(data['camera_booted'])
            self.assertEqual(data['stop']['reason'],'instruction_budget')
            self.assertEqual(data['completed_steps'],2)
            self.assertIn('synthetic',report.read_text().lower())
            self.assertEqual(json.loads((prepared.directory/'dependencies.json').read_text()),[])

    def test_missing_stop_is_explicit(self):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.report'), 'evidence reporting missing')
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            prepared=prepare_run(RESEARCH,Path(tmp)/'unfinished','module-entry-diagnostic')
            s=RunSession(prepared,REPO/'build/qemu-system-arm')
            s.step();s.close()
            write_report(prepared.directory)
            data=json.loads((prepared.directory/'summary.json').read_text())
            self.assertEqual(data['stop']['reason'],'incomplete')
            self.assertFalse(data['camera_booted'])

    def test_cli_relative_research_path(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            path=Path(tmp)/'cli'
            result=subprocess.run([sys.executable,'-m','xt4lab','run','--research-root',
                '../../research/2026-10-14-xt4-firmware','--out',str(path),'--steps','2'],
                cwd=REPO,text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertTrue((path/'report.md').is_file())

    def test_legacy_profile_without_models(self):
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'legacy', 'module-entry-diagnostic')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1)
            s.step(); s.close()
            p = run.directory/'run-info.json'
            info = json.loads(p.read_text())
            info['profile'].pop('models', None)
            p.write_text(json.dumps(info))
            write_report(run.directory)
            data = json.loads((run.directory/'summary.json').read_text())
            self.assertIn('models', data)
            self.assertEqual(data['models'], [])
            self.assertEqual(data['modeled_write_count'], 0)

    def test_port_cli_profile(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            p = Path(tmp)/'cli-ports'
            result = subprocess.run([sys.executable, '-m', 'xt4lab', 'run',
                '--research-root', str(RESEARCH), '--out', str(p), '--profile',
                'bootstrap-port-config-diagnostic', '--steps', '1'],
                cwd=REPO, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads((p/'summary.json').read_text())
            self.assertEqual(data['models'], ['xt4-port-config-v1'])

    def test_input_cli_requires_explicit_scenario(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for index, name in enumerate([None, 'one-ports-v1']):
                p = Path(tmp)/str(index)
                args = [sys.executable, '-m', 'xt4lab', 'run', '--research-root',
                    str(RESEARCH), '--out', str(p), '--profile',
                    'bootstrap-input-exploration', '--steps', '1']
                if name:
                    args += ['--scenario', name]
                result = subprocess.run(args, cwd=REPO, text=True, capture_output=True, timeout=10)
                if not name:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(p.exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    data = json.loads((p/'summary.json').read_text())
                    self.assertEqual(data['scenario']['scenario_id'], name)
                    self.assertEqual(data['execution_class'], 'synthetic_exploration')
                    self.assertIn('synthetic', (p/'report.md').read_text().lower())

    def test_legacy_report_without_scenario(self):
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'legacy-scenario', 'bootstrap-entry-diagnostic')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1)
            s.step(); s.close()
            p = run.directory/'run-info.json'; info = json.loads(p.read_text())
            info['profile'].pop('scenario', None)
            p.write_text(json.dumps(info)); write_report(run.directory)
            data = json.loads((run.directory/'summary.json').read_text())
            self.assertTrue('scenario' in data, 'legacy report lacks scenario compatibility')
            self.assertIsNone(data['scenario'])
            self.assertIsNone(data['scenario_sha256'])
            self.assertEqual(data['execution_class'], 'diagnostic')

    def test_exploration_report_cannot_lose_scenario_identity(self):
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'identity', 'bootstrap-input-exploration',
                              scenario_name='zero-ports-v1')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1)
            s.step(); s.close()
            p = run.directory/'run-info.json'; original = p.read_text()
            for mutation in ['missing', 'hash', 'boolean', 'words']:
                with self.subTest(mutation=mutation):
                    info = json.loads(original)
                    if mutation == 'missing': info['profile'].pop('scenario')
                    elif mutation == 'hash': info['provenance']['scenario_sha256'] = '0' * 64
                    elif mutation == 'boolean': info['profile']['scenario']['port_inputs'][0] = False
                    else: info['profile']['scenario']['port_inputs'][0] = 1
                    p.write_text(json.dumps(info))
                    with self.assertRaises(ValueError):
                        write_report(run.directory)

    def test_scenario_report_bound_to_engine_reset(self):
        from xt4lab.report import write_report
        from xt4lab.scenarios import load_scenario, scenario_dict, scenario_sha256
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'swapped', 'bootstrap-timer-exploration',
                              scenario_name='zero-ports-v1')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1)
            s.step(); s.close()
            p = run.directory/'run-info.json'; info = json.loads(p.read_text())
            # Internally consistent metadata still contradicts the actual C reset.
            other = load_scenario('one-ports-v1', timer=True)
            info['profile']['scenario'] = scenario_dict(other)
            info['provenance']['scenario_sha256'] = scenario_sha256(other)
            p.write_text(json.dumps(info))
            with self.assertRaisesRegex(ValueError, 'engine.*scenario|scenario.*engine'):
                write_report(run.directory)

    def test_synthetic_report_cannot_be_downgraded_to_diagnostic(self):
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'downgraded', 'bootstrap-timer-exploration',
                              scenario_name='zero-ports-v1')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1)
            s.step(); s.close()
            path = run.directory/'run-info.json'; original = path.read_text()
            for name in ['module-entry-diagnostic', 'bootstrap-entry-diagnostic',
                         'bootstrap-port-config-diagnostic']:
                with self.subTest(profile=name):
                    info = json.loads(original)
                    info['profile']['name'] = name
                    info['profile'].pop('scenario')
                    info['profile'].pop('models')
                    info['provenance'].pop('scenario_sha256')
                    path.write_text(json.dumps(info))
                    with self.assertRaisesRegex(ValueError, 'engine'):
                        write_report(run.directory)

    def test_transition_counts_and_hash_keep_virtual_time(self):
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'transition-fixture', 'bootstrap-timer-exploration',
                              scenario_name='zero-ports-v1')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1)
            s.step(); s.close()
            path = run.directory/'segments/000/engine-events.jsonl'
            original = path.read_text()
            # Report-unit fixture; actual asynchronous delivery is tested in test_scenarios.
            event = dict(event='device-transition', device='xt4-timer-exploration-v1',
                operation='terminal-count', success=False, virtual_ns=38, width=None, value=None,
                scenario='zero-ports-v1', detail='unknown_timer_terminal_count')
            path.write_text(original + json.dumps(event) + '\n')
            write_report(run.directory)
            data = json.loads((run.directory/'summary.json').read_text())
            self.assertTrue('modeled_operations_by_model' in data, 'missing per-model operation counts')
            self.assertEqual(data['modeled_operations_by_model']['xt4-timer-exploration-v1'],
                             dict(writes=0, reads=0, transitions=1))
            self.assertEqual((data['modeled_write_count'], data['modeled_read_count']), (0, 0))
            self.assertFalse(data['camera_booted'])
            event['virtual_ns'] = 39
            path.write_text(original + json.dumps(event) + '\n')
            write_report(run.directory)
            changed = json.loads((run.directory/'summary.json').read_text())
            self.assertNotEqual(data['canonical_device_events_sha256'], changed['canonical_device_events_sha256'])

if __name__=='__main__':
    unittest.main()
