"""Scenario admission prevents synthetic states leaking into diagnostics."""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from xt4lab.firmware import load_profile, prepare_run, validate_profile, verify_prepared

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
INPUT = 'bootstrap-input-exploration'


class ScenarioTests(unittest.TestCase):
    def profile(self, scenario='zero-ports-v1'):
        try:
            return load_profile(INPUT, scenario_name=scenario)
        except TypeError as exc:
            self.fail('Explicit scenario selection is missing: ' + str(exc))

    def test_explicit_synthetic_words_and_hash(self):
        for name, value in [('zero-ports-v1', 0), ('one-ports-v1', 0xffffffff)]:
            with self.subTest(name=name):
                p = self.profile(name)
                self.assertEqual(p.scenario.port_inputs, (value,) * 16)
                self.assertIsNone(p.scenario.timer)
                self.assertEqual(p.models, ('xt4-port-config-v1', 'xt4-static-port-input-v1'))
                from xt4lab.scenarios import scenario_dict, scenario_sha256
                plain = scenario_dict(p.scenario)
                self.assertEqual(plain['scenario_id'], name)
                self.assertEqual(scenario_sha256(p.scenario), hashlib.sha256(
                    json.dumps(plain, sort_keys=True, separators=(',', ':')).encode()).hexdigest())

    def test_scenario_required_and_strict_profiles_reject_it(self):
        self.profile()
        with self.assertRaises(ValueError):
            load_profile(INPUT)
        with self.assertRaises(ValueError):
            load_profile(INPUT, scenario_name='normal-camera')
        for name in ['module-entry-diagnostic', 'bootstrap-entry-diagnostic',
                     'bootstrap-port-config-diagnostic']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                load_profile(name, scenario_name='zero-ports-v1')

    def test_exact_identity_words_types_and_model_admission(self):
        p = self.profile()
        for words in [(), (0,) * 15, (0,) * 17, (True,) * 16,
                      (-1,) * 16, (0x100000000,) * 16, (1,) + (0,) * 15]:
            with self.subTest(words=words[:1]), self.assertRaises(ValueError):
                validate_profile(replace(p, scenario=replace(p.scenario, port_inputs=words)))
        for bad in [replace(p.scenario, scenario_id='one-ports-v1'),
                    replace(p.scenario, timer={}), None]:
            with self.subTest(scenario=bad), self.assertRaises(ValueError):
                validate_profile(replace(p, scenario=bad))
        for models in [(), ('xt4-port-config-v1',), p.models[::-1]]:
            with self.subTest(models=models), self.assertRaises(ValueError):
                validate_profile(replace(p, models=models))
        strict = load_profile('bootstrap-port-config-diagnostic')
        with self.assertRaises(ValueError):
            validate_profile(replace(strict, scenario=p.scenario))

    def test_staged_scenario_immutable_and_fingerprinted(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp) / 'input', INPUT,
                              scenario_name='one-ports-v1')
            from xt4lab.scenarios import scenario_sha256
            self.assertEqual(run.manifest['scenario_sha256'], scenario_sha256(run.profile.scenario))
            verify_prepared(run)
            p = run.directory / 'profile.json'
            data = json.loads(p.read_text()); data['scenario']['port_inputs'][0] = 0
            p.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                verify_prepared(run)

    def reject_engine(self, run):
        try:
            result = subprocess.run([str(REPO / 'build/qemu-system-arm'), '-machine',
                'fujifilm-xt4,run-dir=' + str(run.directory), '-display', 'none',
                '-nodefaults', '-S'], capture_output=True, timeout=2)
        except subprocess.TimeoutExpired:
            self.fail('C machine accepted an invalid scenario')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((run.directory / 'engine-events.jsonl').exists())

    def test_c_strict_profile_rejects_synthetic_scenario(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp) / 'strict', 'bootstrap-port-config-diagnostic')
            p = run.directory / 'profile.json'; data = json.loads(p.read_text())
            data['scenario'] = {'scenario_id': 'zero-ports-v1', 'port_inputs': [0] * 16, 'timer': None}
            p.write_text(json.dumps(data)); self.reject_engine(run)

    def test_c_independently_rejects_malformed_scenarios(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            for i, field in enumerate(['missing', 'identity', 'length', 'word', 'boolean', 'timer', 'models']):
                with self.subTest(field=field):
                    run = prepare_run(RESEARCH, Path(tmp) / str(i), INPUT, scenario_name='zero-ports-v1')
                    p = run.directory / 'profile.json'; data = json.loads(p.read_text())
                    if field == 'missing': data.pop('scenario')
                    elif field == 'identity': data['scenario']['scenario_id'] = 'one-ports-v1'
                    elif field == 'length': data['scenario']['port_inputs'].pop()
                    elif field == 'word': data['scenario']['port_inputs'][0] = -1
                    elif field == 'boolean': data['scenario']['port_inputs'][0] = False
                    elif field == 'timer': data['scenario']['timer'] = {}
                    else: data['models'].pop()
                    p.write_text(json.dumps(data)); self.reject_engine(run)


class TimerAdmissionTests(unittest.TestCase):
    def profile(self):
        try:
            return load_profile('bootstrap-timer-exploration', scenario_name='zero-ports-v1')
        except ValueError as exc:
            self.fail('Timer exploration profile is missing: ' + str(exc))

    def test_exact_clock_types_and_model_set(self):
        p = self.profile()
        self.assertEqual(p.models, ('xt4-port-config-v1', 'xt4-static-port-input-v1',
                                    'xt4-timer-exploration-v1'))
        self.assertEqual((p.scenario.timer.frequency_hz, p.scenario.timer.icount_shift), (54000000, 0))
        for field, value in [('frequency_hz', 54000001), ('frequency_hz', True),
                             ('initial_halted', False), ('initial_halted', 1),
                             ('enable_preserves_count', False), ('icount_shift', False),
                             ('icount_shift', 1), ('stop_at_terminal_count', False)]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                validate_profile(replace(p, scenario=replace(p.scenario,
                    timer=replace(p.scenario.timer, **{field:value}))))

    def test_c_rejects_clock_overrides_and_malformed_metadata(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            cases = [('missing-clock', []), ('shift', ['-icount', 'shift=1']),
                     ('adaptive', ['-icount', 'shift=auto']),
                     ('frequency', ['-icount', 'shift=0,align=off,sleep=off']),
                     ('boolean', ['-icount', 'shift=0,align=off,sleep=off']),
                     ('mode', ['-icount', 'shift=0,align=off,sleep=off'])]
            for i, (name, extra) in enumerate(cases):
                with self.subTest(name=name):
                    run = prepare_run(RESEARCH, Path(tmp)/str(i), 'bootstrap-timer-exploration',
                                      scenario_name='zero-ports-v1')
                    p = run.directory/'profile.json'; data=json.loads(p.read_text())
                    if name=='frequency':data['scenario']['timer']['frequency_hz']=1
                    if name=='boolean':data['scenario']['timer']['icount_shift']=False
                    if name=='mode':data['scenario']['timer']['mode']=3
                    p.write_text(json.dumps(data))
                    try:
                        result=subprocess.run([str(REPO/'build/qemu-system-arm'), '-machine',
                            'fujifilm-xt4,run-dir='+str(run.directory), '-accel','tcg,thread=single',
                            '-nodefaults','-display','none','-S',*extra],capture_output=True,timeout=2)
                    except subprocess.TimeoutExpired:
                        self.fail('Engine accepted unsupported clock/scenario')
                    self.assertNotEqual(result.returncode,0)
                    self.assertFalse((run.directory/'engine-events.jsonl').exists())

    def test_runner_clock_only_on_timer_profile(self):
        self.profile()
        from xt4lab.run import RunSession
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for i, name in enumerate(['bootstrap-input-exploration','bootstrap-timer-exploration']):
                run=prepare_run(RESEARCH,Path(tmp)/str(i),name,scenario_name='one-ports-v1')
                s=RunSession(run,REPO/'build/qemu-system-arm',budget=1)
                try:
                    if i:
                        self.assertEqual(s.argv[s.argv.index('-icount')+1], 'shift=0,align=off,sleep=off')
                    else:self.assertNotIn('-icount',s.argv)
                    self.assertEqual(s.continue_steps(1)['reason'],'instruction_budget')
                finally:s.close()

    def test_terminal_transition_in_report(self):
        self.profile()
        from xt4lab.run import RunSession
        from xt4lab.report import write_report
        import struct
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'terminal','bootstrap-timer-exploration',
                            scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=150)
            # Test-only guest RAM program. No authentic run or source is patched.
            code=[0xe30c0310,0xe34f0ff3,0xe3a01002,0xe5801000,
                  0xe30c0300,0xe34f0ff3,0xe3001001,0xe3401001,0xe5801000]
            code += [0xe320f000]*80+[0xe30a4000,0xe3404001,0xe5841000,0xeafffffe]
            try:
                raw=struct.pack('<'+'I'*len(code),*code)
                self.assertEqual(s.client.command('M0,'+format(len(raw),'x')+':'+raw.hex()),'OK')
                result=s.continue_steps(150)
                self.assertEqual(result['reason'],'unsupported_device_transition',result)
                self.assertEqual(result['detail'],'unknown_timer_terminal_count')
                self.assertIsNone(result['width']);self.assertIsNone(result['value'])
                self.assertEqual(s.client.read_memory(0x1a000, 4), bytes(4))
            finally:s.close()
            write_report(run.directory)
            summary=json.loads((run.directory/'summary.json').read_text())
            events=json.loads((run.directory/'device-events.json').read_text())
            self.assertTrue(any(e['event']=='device-transition' for e in events))
            self.assertFalse(summary['camera_booted'])

    def test_debugger_pause_does_not_warp_timer(self):
        self.profile()
        from xt4lab.run import RunSession
        import struct
        import time
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'pause-clock','bootstrap-timer-exploration',
                            scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=100)
            code=[0xe30c0310,0xe34f0ff3,0xe30017d0,0xe5801000,
                  0xe30c0300,0xe34f0ff3,0xe3001001,0xe3401001,0xe5801000]
            code += [0xe320f000]*80+[0xeafffffe]
            try:
                raw=struct.pack('<'+'I'*len(code),*code)
                self.assertEqual(s.client.command('M0,'+format(len(raw),'x')+':'+raw.hex()),'OK')
                self.assertEqual(s.continue_steps(9)['reason'],'step')
                time.sleep(.02)
                self.assertEqual(struct.unpack('<I',s.client.read_memory(0xfff3c310,4))[0],2000)
                self.assertEqual(s.continue_steps(10)['reason'],'step')
                self.assertEqual(struct.unpack('<I',s.client.read_memory(0xfff3c310,4))[0],2000)
            finally:s.close()
