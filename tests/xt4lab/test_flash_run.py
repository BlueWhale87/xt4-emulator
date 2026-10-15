"""Typed virtual flash identity and original bootstrap integration."""
from dataclasses import replace, fields
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from xt4lab.firmware import load_profile, prepare_run, profile_dict, validate_profile
from xt4lab.scenarios import scenario_dict, scenario_from_dict, scenario_sha256
from xt4lab.run import RunSession
from xt4lab.report import write_report

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
PROFILE = 'bootstrap-flash-exploration'
FLASH_MODELS = ('xt4-cadence-stig-exploration-v1', 'mx25um51245gxdi00-boot-v1')


class FlashAdmissionTests(unittest.TestCase):
    def test_explicit_exact_typed_contract(self):
        with self.assertRaises(ValueError): load_profile(PROFILE)
        for name in ('zero-ports-v1', 'one-ports-v1'):
            p = load_profile(PROFILE, scenario_name=name)
            self.assertEqual(p.models[-2:], FLASH_MODELS)
            flash = p.scenario.flash
            self.assertEqual(flash.virtual_part, FLASH_MODELS[1])
            self.assertEqual((flash.transaction_latency_ns, flash.cr2_typical_settle_ns), (64, 40))
            plain = scenario_dict(p.scenario)
            self.assertEqual(scenario_from_dict(plain, timer=True, platform=True, flash=True), p.scenario)
            for field in fields(flash):
                original = getattr(flash, field.name)
                invalid = [None, False, 1.0] if type(original) is int else [None, 0, 'other']
                for value in invalid:
                    with self.subTest(field=field.name, value=value), self.assertRaises(ValueError):
                        validate_profile(replace(p, scenario=replace(p.scenario,
                            flash=replace(flash, **{field.name: value}))))
                bad = json.loads(json.dumps(plain)); bad['flash'].pop(field.name)
                with self.assertRaises(ValueError): scenario_from_dict(bad, timer=True, platform=True, flash=True)
            for value in (None, {}, {**plain['flash'], 'extra': 0}):
                with self.assertRaises(ValueError):
                    scenario_from_dict({**plain, 'flash': value}, timer=True, platform=True, flash=True)
            old = load_profile('bootstrap-controller-exploration', scenario_name=name)
            self.assertNotIn('flash', scenario_dict(old.scenario))
            with self.assertRaises(ValueError):
                validate_profile(replace(old, scenario=replace(old.scenario, flash=flash)))
            with self.assertRaises(ValueError):
                scenario_from_dict({**scenario_dict(old.scenario), 'flash': None}, timer=True, platform=True)

    def test_c_rejects_each_flash_field_and_extra_or_old_identity(self):
        p = load_profile(PROFILE, scenario_name='zero-ports-v1')
        plain = scenario_dict(p.scenario)
        mutations = []
        for key, value in plain['flash'].items():
            mutations += [(key, None), (key, False if type(value) is int else 0)]
            if type(value) is int: mutations += [(key, float(value)), (key, value + 1)]
        mutations += [('extra', 0), ('whole', None), ('old', None)]
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for i, (key, value) in enumerate(mutations):
                run = prepare_run(RESEARCH, Path(tmp)/str(i), PROFILE, scenario_name='zero-ports-v1')
                path = run.directory/'profile.json'; data = json.loads(path.read_text())
                if key == 'whole': data['scenario'].pop('flash')
                elif key == 'old':
                    data['name'] = 'bootstrap-controller-exploration'; data['models'] = data['models'][:-2]
                elif value is None: data['scenario']['flash'].pop(key)
                else: data['scenario']['flash'][key] = value
                path.write_text(json.dumps(data))
                result = subprocess.run([str(REPO/'build/qemu-system-arm'), '-machine',
                    'fujifilm-xt4,run-dir='+str(run.directory), '-S', '-nodefaults', '-display', 'none',
                    '-accel', 'tcg,thread=single', '-icount', 'shift=0,align=off,sleep=off'],
                    capture_output=True, timeout=2)
                with self.subTest(key=key, value=value):
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((run.directory/'engine-events.jsonl').exists())

    def test_c_hash_and_report_cannot_downgrade_flash(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp)/'identity', PROFILE, scenario_name='zero-ports-v1')
            s = RunSession(run, REPO/'build/qemu-system-arm', budget=1, execution_mode='bounded-continue')
            try: s.continue_fast()
            finally: s.close()
            write_report(run.directory)
            events = [json.loads(line) for line in (run.directory/'segments/000/engine-events.jsonl').read_text().splitlines()]
            reset = next(e for e in events if e['event']=='reset')
            self.assertEqual(reset['scenario_sha256'], scenario_sha256(run.profile.scenario))
            path = run.directory/'run-info.json'; data = json.loads(path.read_text())
            old = load_profile('bootstrap-controller-exploration', scenario_name='zero-ports-v1')
            data['profile'] = profile_dict(old)
            data['provenance']['scenario_sha256'] = scenario_sha256(old.scenario)
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, 'engine'): write_report(run.directory)
            older = prepare_run(RESEARCH, Path(tmp)/'older', old.name, scenario_name='zero-ports-v1')
            s = RunSession(older, REPO/'build/qemu-system-arm', budget=1, execution_mode='bounded-continue')
            try: s.continue_fast()
            finally: s.close()
            path=older.directory/'run-info.json'; data=json.loads(path.read_text())
            data['profile']=profile_dict(run.profile)
            data['provenance']['scenario_sha256']=scenario_sha256(run.profile.scenario)
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, 'engine'): write_report(older.directory)


class FlashRunTests(unittest.TestCase):
    def run_original(self, root, name, scenario):
        run = prepare_run(RESEARCH, root/name, PROFILE, scenario_name=scenario)
        original = (run.directory/'inputs/bootstrap.bin').read_bytes()
        s = RunSession(run, REPO/'build/qemu-system-arm', budget=100000,
                       execution_mode='bounded-continue')
        try:
            result = s.continue_fast()
            self.assertEqual((result['reason'], result['pc'], result['physical_address'], result['width'],
                              result['value'], result['device'], result['detail']),
                ('unsupported_access', 0x4f34, 0xfffd8000, 4, 0xc10000c9, FLASH_MODELS[0],
                 'unsupported_flash_config'))
            self.assertEqual(s.client.read_memory(0x10adc, 8), bytes.fromhex('c2000000bcc10000'))
            self.assertEqual(s.client.read_memory(0x10ae8, 4), bytes.fromhex('80a002f0'))
        finally: s.close()
        write_report(run.directory)
        summary = json.loads((run.directory/'summary.json').read_text())
        events = json.loads((run.directory/'device-events.json').read_text())
        completions = [e for e in events if e['device']==FLASH_MODELS[1]]
        self.assertEqual([(e['opcode'], e['rx_len'], e['protocol'], e['write_enabled']) for e in completions],
            [(0x9f,1,'spi',False), (6,0,'spi',True), (5,1,'spi',True),
             (0x72,0,'opi-dtr',False), (5,2,'opi-dtr',False)])
        commands = [e for e in events if e['device']==FLASH_MODELS[0] and
            e['operation']=='write' and e['address']==0xfffd8090 and e['value'] & 1]
        self.assertEqual([e['virtual_ns']-c['virtual_ns'] for e,c in zip(completions,commands)],
                         [64,64,64,104,64])
        writes = [(e['address'],e['value']) for e in events if e['device']==FLASH_MODELS[0]
                  and e['operation']=='write' and e['success']]
        final = [(0xfffd8000,0x410800c1),(0xfffd8004,0xee),(0xfffd8008,0x12),
            (0xfffd800c,0x08080808),(0xfffd8020,0x606),(0xfffd80b4,0x80200020),
            (0xfffd80e0,0x11edfa00),(0xfffd80f0,0x10)]
        start = writes.index(final[0]); self.assertEqual(writes[start:start+8], final)
        self.assertEqual(summary['modeled_operations_by_model'][FLASH_MODELS[1]],
                         dict(reads=0,writes=0,transitions=5))
        self.assertEqual((summary['completed_steps'], summary['engine_instruction_count']), (0,23565))
        self.assertFalse(summary['camera_booted'])
        for path in ('inputs/bootstrap.bin','segments/000/inputs/bootstrap.bin'):
            self.assertEqual((run.directory/path).read_bytes(), original)
        return run,summary

    def test_original_oracle_both_scenarios_and_repeat(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            root=Path(tmp)
            _,a=self.run_original(root,'zero','zero-ports-v1')
            _,b=self.run_original(root,'one','one-ports-v1')
            _,c=self.run_original(root,'repeat','zero-ports-v1')
            for key in ('canonical_trace_sha256','canonical_device_events_sha256','engine_instruction_count'):
                self.assertEqual(a[key],c[key])
            self.assertEqual(a['engine_instruction_count'],b['engine_instruction_count'])
            self.assertNotEqual(a['canonical_device_events_sha256'],b['canonical_device_events_sha256'])

    def test_pending_debug_pause_resume_and_fresh_engine_reset(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'pending',PROFILE,scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=1000,execution_mode='bounded-continue')
            def mem(addr, data):
                self.assertEqual(s.client.command(f'M{addr:x},{len(data):x}:'+data.hex()),'OK')
            def nor_count():
                events=[json.loads(line) for line in (s.segment_dir/'engine-events.jsonl').read_text().splitlines()]
                return sum(e.get('event')=='device-transition' and e.get('device')==FLASH_MODELS[1] for e in events)
            try:
                # Pause at an actual software breakpoint after16 instructions,
                # before the64ns transaction deadline; no synthetic ready input.
                mem(0,bytes.fromhex('0000a0e1')*16+bytes.fromhex('feffffea'))
                mem(0xfffd8000,(0x800c1).to_bytes(4,'little'))
                mem(0xfffd8090,(0x06000001).to_bytes(4,'little'))
                self.assertEqual(s.client.command('Z0,40,4'),'OK')
                reply=s.client.continue_run(); self.assertEqual(reply.signal,5)
                self.assertEqual(nor_count(),0)
                before=(s.segment_dir/'engine-events.jsonl').read_bytes()
                time.sleep(0.02)
                self.assertEqual((s.segment_dir/'engine-events.jsonl').read_bytes(),before)
                self.assertEqual(int.from_bytes(s.client.read_memory(0xfffd8090,4),'little')&2,2)
                self.assertEqual(s.client.command('z0,40,4'),'OK')
                s.continue_fast(); self.assertEqual(nor_count(),1)
                s.reset(); self.assertEqual(nor_count(),0)
                self.assertEqual(s.client.read_memory(0xfffd8090,4),bytes(4))
                # A reset while pending destroys the callback and effect; the
                # next engine runs original firmware from a fresh reset state.
                mem(0xfffd8000,(0x800c1).to_bytes(4,'little'))
                mem(0xfffd8090,(0x06000001).to_bytes(4,'little'))
                s.reset(); self.assertEqual(nor_count(),0)
                s.continue_fast(); self.assertEqual(nor_count(),0)
            finally:s.close()


if __name__ == '__main__': unittest.main()
