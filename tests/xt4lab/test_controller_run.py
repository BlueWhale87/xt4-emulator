"""Bounded controller exploration, independent startup oracle and old identities."""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from xt4lab.firmware import load_profile, prepare_run, profile_dict, validate_profile
from xt4lab.report import write_report
from xt4lab.run import RunSession
from xt4lab.scenarios import scenario_dict, scenario_from_dict, scenario_sha256

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
PROFILE = 'bootstrap-controller-exploration'
MODELS = ('xt4-port-config-v1', 'xt4-static-port-input-v1', 'xt4-timer-exploration-v1',
          'xt4-transfer-enable-exploration-v1', 'xt4-clock-apply-exploration-v1')


class ControllerAdmissionTests(unittest.TestCase):
    def profile(self, name='zero-ports-v1'):
        try:
            return load_profile(PROFILE, scenario_name=name)
        except ValueError as exc:
            self.fail('Controller exploration profile missing: ' + str(exc))

    def test_old_scenario_and_profile_identities_stay_exact(self):
        expected = [
            ('bootstrap-input-exploration', 'zero-ports-v1',
             '6b646da0e6309b47c6f1bb6b7a6ea722a3e7aebd27711da0227b310ec6416a9a',
             '65bc709373b867a8793b3505ef3af4c9898110e5fea0471decf3e905af2f19c1'),
            ('bootstrap-input-exploration', 'one-ports-v1',
             'a9d4a538d3a54ba5a00e2e0aeaf6cba741ff225c45aad3d929d42e6b7da83b92',
             '631df8c11732299fe810013ac63503c22582a165b7c0161fd1a5e7bb9277f051'),
            ('bootstrap-timer-exploration', 'zero-ports-v1',
             '432a84d9c836644fc6da807828a9491791148f04e7a3f80ab9aacb92cfb0c9d0',
             '764755cba2e3467c95dafb597f427e2bbb16e93909dc64e6eca3083671378b32'),
            ('bootstrap-timer-exploration', 'one-ports-v1',
             '0e944eb256af720a5da7bf81a360f860161b223fc4bfb15c806f8340e2addbe5',
             'a4f9494bc4543491a91242793fae6ed07595212c1bd8a83c1553872d9f7734f7')]
        for profile, name, scenario_hash, profile_hash in expected:
            with self.subTest(profile=profile, scenario=name):
                p = load_profile(profile, scenario_name=name)
                self.assertNotIn('platform', scenario_dict(p.scenario))
                self.assertEqual(scenario_sha256(p.scenario), scenario_hash)
                self.assertEqual(hashlib.sha256(json.dumps(profile_dict(p), sort_keys=True).encode()).hexdigest(), profile_hash)

    def test_explicit_platform_and_exact_typed_identity(self):
        for name, word in [('zero-ports-v1', 0), ('one-ports-v1', 0xffffffff)]:
            p = self.profile(name)
            self.assertEqual(p.models, MODELS)
            self.assertEqual(p.scenario.platform.transfer_globals, (word,) * 16)
            self.assertEqual(p.scenario.platform.clock_apply_latency_ns, 1)
            plain = scenario_dict(p.scenario)
            self.assertEqual(scenario_from_dict(plain, timer=True, platform=True), p.scenario)
            for field, value in [('transfer_globals', (word,) * 15),
                                 ('transfer_globals', (False,) * 16),
                                 ('transfer_globals', (float(word),) * 16),
                                 ('transfer_globals', (1,) * 16),
                                 ('clock_apply_latency_ns', True),
                                 ('clock_apply_latency_ns', 1.0),
                                 ('clock_apply_latency_ns', 0),
                                 ('clock_apply_latency_ns', 2)]:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    validate_profile(replace(p, scenario=replace(p.scenario,
                        platform=replace(p.scenario.platform, **{field: value}))))
            for field, value in [('clock_apply_latency_ns', True), ('clock_apply_latency_ns', 1.0),
                                 ('transfer_globals', [False] * 16)]:
                bad = json.loads(json.dumps(plain)); bad['platform'][field] = value
                with self.assertRaises(ValueError): scenario_from_dict(bad, timer=True, platform=True)
        with self.assertRaises(ValueError): load_profile(PROFILE)

    def test_platform_cannot_leak_into_older_profiles(self):
        p = self.profile()
        for name in ['bootstrap-input-exploration', 'bootstrap-timer-exploration']:
            old = load_profile(name, scenario_name='zero-ports-v1')
            with self.assertRaises(ValueError):
                validate_profile(replace(old, scenario=replace(old.scenario, platform=p.scenario.platform)))
        with self.assertRaises(ValueError): validate_profile(replace(p, models=MODELS[:-1]))
        with self.assertRaises(ValueError): validate_profile(replace(p, scenario=replace(p.scenario, platform=None)))

    def test_c_independently_rejects_platform_mutations(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            for i, mutation in enumerate(['missing', 'word', 'boolean', 'float', 'length',
                                           'latency-bool', 'latency-float', 'latency', 'extra', 'models', 'old-profile']):
                with self.subTest(mutation=mutation):
                    run = prepare_run(RESEARCH, Path(tmp)/str(i), PROFILE, scenario_name='zero-ports-v1')
                    path = run.directory/'profile.json'; d = json.loads(path.read_text())
                    platform = d['scenario']['platform']
                    if mutation == 'missing': d['scenario'].pop('platform')
                    elif mutation == 'word': platform['transfer_globals'][0] = 1
                    elif mutation == 'boolean': platform['transfer_globals'][0] = False
                    elif mutation == 'float': platform['transfer_globals'][0] = 0.0
                    elif mutation == 'length': platform['transfer_globals'].pop()
                    elif mutation == 'latency-bool': platform['clock_apply_latency_ns'] = True
                    elif mutation == 'latency-float': platform['clock_apply_latency_ns'] = 1.0
                    elif mutation == 'latency': platform['clock_apply_latency_ns'] = 2
                    elif mutation == 'extra': platform['status'] = 0
                    elif mutation == 'models': d['models'].pop()
                    else:
                        d['name'] = 'bootstrap-timer-exploration'; d['models'] = list(MODELS[:3])
                    path.write_text(json.dumps(d))
                    result = subprocess.run([str(REPO/'build/qemu-system-arm'), '-machine',
                        'fujifilm-xt4,run-dir='+str(run.directory), '-S', '-accel', 'tcg,thread=single',
                        '-icount', 'shift=0,align=off,sleep=off', '-nodefaults', '-display', 'none'],
                        capture_output=True, timeout=2)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((run.directory/'engine-events.jsonl').exists())


class ControllerRunTests(unittest.TestCase):
    def run_original(self, root, name, scenario):
        run = prepare_run(RESEARCH, root/name, PROFILE, scenario_name=scenario)
        original = (run.directory/'inputs/bootstrap.bin').read_bytes()
        s = RunSession(run, REPO/'build/qemu-system-arm', budget=100000, execution_mode='bounded-continue')
        try:
            result = s.continue_fast()
            self.assertEqual((result['reason'], result['pc'], result['physical_address'], result['value'], result['width']),
                             ('unsupported_access', 0x4f34, 0xfffd8000, 0x000800c1, 4))
            self.assertNotIn('device', result)
            self.assertNotIn('detail', result)
        finally: s.close()
        write_report(run.directory)
        d = json.loads((run.directory/'summary.json').read_text())
        events = json.loads((run.directory/'device-events.json').read_text())
        transfer = [e for e in events if e['device'] == MODELS[3]]
        seed = 0 if scenario == 'zero-ports-v1' else 0xffffffff
        self.assertEqual([(e['operation'], e['address'], e['value']) for e in transfer],
            [(op, 0xfffe0000+i*0x1000, seed if op == 'read' else seed | 0x10000000)
             for i in range(16) for op in ['read', 'write']])
        clock = [e for e in events if e['device'] == MODELS[4]]
        self.assertEqual([(e['address'],e['value']) for e in clock if e['operation'] == 'write'],
                         [(0xff701040,0x101),(0xff701080,2),(0xff701048,0x107),(0xff701080,2)])
        transitions = [e for e in clock if e['event']=='device-transition']
        self.assertEqual(len(transitions),2)
        commands = [e for e in clock if e['operation']=='write' and e['address']==0xff701080]
        self.assertEqual([e['virtual_ns']-c['virtual_ns'] for e,c in zip(transitions,commands)], [1,1])
        self.assertTrue(all(e['success'] for e in events))
        self.assertEqual(d['modeled_operations_by_model'][MODELS[3]],dict(writes=16,reads=16,transitions=0))
        self.assertEqual(d['modeled_operations_by_model'][MODELS[4]]['transitions'],2)
        self.assertEqual(d['completed_steps'],0)
        self.assertFalse(d['camera_booted'])
        self.assertEqual((run.directory/'inputs/bootstrap.bin').read_bytes(),original)
        self.assertEqual((run.directory/'segments/000/inputs/bootstrap.bin').read_bytes(),original)
        return run,d

    def test_original_oracle_both_scenarios_repeat_and_identity(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            root = Path(tmp)
            a, first = self.run_original(root,'zero','zero-ports-v1')
            _, other = self.run_original(root,'one','one-ports-v1')
            _, repeat = self.run_original(root,'repeat','zero-ports-v1')
            for key in ['canonical_trace_sha256','canonical_device_events_sha256','engine_instruction_count']:
                self.assertEqual(first[key],repeat[key])
            self.assertEqual(first['engine_instruction_count'],other['engine_instruction_count'])
            self.assertNotEqual(first['canonical_device_events_sha256'],other['canonical_device_events_sha256'])
            path=a.directory/'run-info.json'; d=json.loads(path.read_text())
            d['profile']['name']='bootstrap-timer-exploration'; d['profile']['models']=list(MODELS[:3])
            d['profile']['scenario'].pop('platform')
            d['provenance']['scenario_sha256']='432a84d9c836644fc6da807828a9491791148f04e7a3f80ab9aacb92cfb0c9d0'
            path.write_text(json.dumps(d))
            with self.assertRaisesRegex(ValueError,'engine'): write_report(a.directory)

    def test_reset_latest_segment_and_cancel_pending_state(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH,Path(tmp)/'reset',PROFILE,scenario_name='zero-ports-v1')
            s = RunSession(run,REPO/'build/qemu-system-arm',budget=100000,execution_mode='bounded-continue')
            try:
                s.continue_fast(); write_report(run.directory)
                first=json.loads((run.directory/'summary.json').read_text())
                old=(s.segment_dir/'engine-events.jsonl').read_bytes()
                s.reset(); write_report(run.directory)
                fresh=json.loads((run.directory/'summary.json').read_text())
                self.assertEqual((fresh['modeled_write_count'],fresh['modeled_read_count']),(0,0))
                self.assertEqual(fresh['engine_instruction_count'],0)
                s.continue_fast()
            finally:s.close()
            write_report(run.directory)
            after=json.loads((run.directory/'summary.json').read_text())
            self.assertEqual(first['canonical_device_events_sha256'],after['canonical_device_events_sha256'])
            self.assertEqual((run.directory/'segments/000/engine-events.jsonl').read_bytes(),old)
            self.assertEqual(after['stop']['segment'],1)


if __name__=='__main__': unittest.main()
