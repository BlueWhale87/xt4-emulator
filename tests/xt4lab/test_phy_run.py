"""Independent typed PHY scenario and unchanged original boot continuation."""
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

REPO=Path(__file__).resolve().parents[2]
RESEARCH=REPO.parents[1]/'research/2026-10-14-xt4-firmware'
PROFILE='bootstrap-phy-exploration'
PHY_MODEL='xt4-cadence-phy-exploration-v1'
FLASH_MODEL='xt4-cadence-stig-exploration-v1'


class PhyAdmissionTests(unittest.TestCase):
    def test_previous_flash_identities_stay_exact(self):
        expected=[('zero-ports-v1','b7b5f202e94daae1850fc16bad0e27a0968e9eb1b0a2ffa1cc4fcac2c74f8694',
                   '0f3daf0249fef7ba20d5db1465e2a8f8af858076dbbb8b1ee994602f6a141a85'),
                  ('one-ports-v1','29304459bd1c6e571dc4ea002baaa0d855de158f296886567c9859837f3f4966',
                   '215ebdbe2a2c3f8fd394f657f550232dac5c90aa4e07b5fea51883db1951025b')]
        for name,scenario_hash,profile_hash in expected:
            p=load_profile('bootstrap-flash-exploration',scenario_name=name)
            self.assertEqual(scenario_sha256(p.scenario),scenario_hash)
            self.assertEqual(hashlib.sha256(json.dumps(profile_dict(p),sort_keys=True).encode()).hexdigest(),profile_hash)
            self.assertNotIn('phy',scenario_dict(p.scenario))

    def test_exact_typed_phy_contract_and_older_refusal(self):
        with self.assertRaises(ValueError):load_profile(PROFILE)
        for name in ('zero-ports-v1','one-ports-v1'):
            p=load_profile(PROFILE,scenario_name=name);phy=p.scenario.phy
            self.assertEqual(p.models[-1],PHY_MODEL)
            self.assertEqual((phy.master_lock_latency_ns,phy.dll_lock_latency_ns,phy.initial_observable_word),(4,4,0))
            plain=scenario_dict(p.scenario)
            self.assertEqual(scenario_from_dict(plain,timer=True,platform=True,flash=True,phy=True),p.scenario)
            for field in fields(phy):
                value=getattr(phy,field.name)
                for bad in (None,False,1.0,'other'):
                    with self.subTest(field=field.name,bad=bad),self.assertRaises(ValueError):
                        validate_profile(replace(p,scenario=replace(p.scenario,phy=replace(phy,**{field.name:bad}))))
                d=json.loads(json.dumps(plain));d['phy'].pop(field.name)
                with self.assertRaises(ValueError):scenario_from_dict(d,timer=True,platform=True,flash=True,phy=True)
            for name in ('bootstrap-flash-exploration','bootstrap-controller-exploration','bootstrap-timer-exploration'):
                old=load_profile(name,scenario_name='zero-ports-v1')
                with self.assertRaises(ValueError):validate_profile(replace(old,scenario=replace(old.scenario,phy=phy)))
            for bad in (None,{}, {**plain['phy'],'extra':0}):
                with self.assertRaises(ValueError):scenario_from_dict({**plain,'phy':bad},timer=True,platform=True,flash=True,phy=True)
            with self.assertRaises(ValueError):scenario_from_dict({**scenario_dict(old.scenario),'phy':None},timer=True)

    def test_c_independent_exact_phy_admission(self):
        p=load_profile(PROFILE,scenario_name='zero-ports-v1');plain=scenario_dict(p.scenario)
        mutations=[]
        for key,value in plain['phy'].items():
            mutations += [(key,None),(key,False),(key,float(value)) if type(value) is int else (key,'other')]
        mutations += [('extra',0),('whole',None),('old',None)]
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for i,(key,value) in enumerate(mutations):
                run=prepare_run(RESEARCH,Path(tmp)/str(i),PROFILE,scenario_name='zero-ports-v1')
                path=run.directory/'profile.json';d=json.loads(path.read_text())
                if key=='whole':d['scenario'].pop('phy')
                elif key=='old':d['name']='bootstrap-flash-exploration';d['models'].pop()
                elif value is None:d['scenario']['phy'].pop(key)
                else:d['scenario']['phy'][key]=value
                path.write_text(json.dumps(d))
                r=subprocess.run([str(REPO/'build/qemu-system-arm'),'-machine','fujifilm-xt4,run-dir='+str(run.directory),
                    '-S','-nodefaults','-display','none','-accel','tcg,thread=single','-icount','shift=0,align=off,sleep=off'],
                    capture_output=True,timeout=2)
                with self.subTest(key=key,value=value):
                    self.assertNotEqual(r.returncode,0)
                    self.assertFalse((run.directory/'engine-events.jsonl').exists())

    def test_report_hash_and_both_identity_directions(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for i,(actual,forged) in enumerate([(PROFILE,'bootstrap-flash-exploration'),('bootstrap-flash-exploration',PROFILE)]):
                run=prepare_run(RESEARCH,Path(tmp)/str(i),actual,scenario_name='zero-ports-v1')
                s=RunSession(run,REPO/'build/qemu-system-arm',budget=1,execution_mode='bounded-continue')
                try:
                    r=s.continue_fast();self.assertEqual(r['reason'],'instruction_budget')
                    self.assertEqual(r['engine_instruction_count'],1)
                finally:s.close()
                write_report(run.directory)
                d=json.loads((run.directory/'run-info.json').read_text())
                other=load_profile(forged,scenario_name='zero-ports-v1')
                d['profile']=profile_dict(other);d['provenance']['scenario_sha256']=scenario_sha256(other.scenario)
                (run.directory/'run-info.json').write_text(json.dumps(d))
                with self.assertRaisesRegex(ValueError,'engine'):write_report(run.directory)


class PhyRunTests(unittest.TestCase):
    def original(self,root,name,scenario):
        run=prepare_run(RESEARCH,root/name,PROFILE,scenario_name=scenario)
        original=(run.directory/'inputs/bootstrap.bin').read_bytes()
        s=RunSession(run,REPO/'build/qemu-system-arm',budget=100000,execution_mode='bounded-continue')
        try:
            result=s.continue_fast()
            self.assertEqual((result['reason'],result['pc'],result['physical_address'],result['value'],result['width']),
                             ('unsupported_access',0x4f34,0xfff4a500,0x01002a2c,4))
            self.assertNotIn('device',result);self.assertNotIn('detail',result)
            self.assertEqual(s.client.read_memory(0x10adc,8),bytes.fromhex('c2000000bcc10000'))
            self.assertEqual(s.client.read_memory(0xca00,128),bytes(128))
        finally:s.close()
        write_report(run.directory)
        summary=json.loads((run.directory/'summary.json').read_text())
        events=json.loads((run.directory/'device-events.json').read_text())
        phy=[e for e in events if e['device']==PHY_MODEL]
        self.assertEqual([(e['address'],e['value']) for e in phy if e['operation']=='write'],
            [(0xfffd80b4,0x80200020),(0xfffd80b4,0),(0xfffd80b8,30),
             (0xfffd80b4,0x40000000),(0xfffd80b4,0xc01f0000)])
        self.assertEqual([e['value'] for e in phy if e['operation']=='read'],[0,0x8000,0x8001])
        transitions=[e for e in phy if e['event']=='device-transition']
        commands=[e for e in phy if e['operation']=='write' and e['value'] in (0x40000000,0xc01f0000)]
        self.assertEqual([e['phase'] for e in transitions],['master-ready','dll-locked'])
        self.assertEqual([e['virtual_ns']-c['virtual_ns'] for e,c in zip(transitions,commands)],[4,4])
        self.assertEqual(summary['modeled_operations_by_model'][PHY_MODEL],dict(writes=5,reads=3,transitions=2))
        nor=[e for e in events if e['device']=='mx25um51245gxdi00-boot-v1']
        self.assertEqual([e['opcode'] for e in nor],[0x9f,6,5,0x72,5])
        fl=[e for e in events if e['device']==FLASH_MODEL]
        self.assertIn((0xfffd8004,0x140333ee),[(e['address'],e['value']) for e in fl if e['operation']=='write'])
        self.assertEqual((summary['engine_instruction_count'],summary['modeled_write_count'],summary['modeled_read_count']),
                         (23842,204,128))
        self.assertFalse(summary['camera_booted'])
        for path in ('inputs/bootstrap.bin','segments/000/inputs/bootstrap.bin'):
            self.assertEqual((run.directory/path).read_bytes(),original)
        return summary

    def test_original_continuation_both_fixtures_and_repeat(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            root=Path(tmp)
            a=self.original(root,'zero','zero-ports-v1')
            b=self.original(root,'one','one-ports-v1')
            c=self.original(root,'repeat','zero-ports-v1')
            for key in ('canonical_device_events_sha256','canonical_trace_sha256','engine_instruction_count'):
                self.assertEqual(a[key],c[key])
            self.assertEqual(a['engine_instruction_count'],b['engine_instruction_count'])
            self.assertNotEqual(a['canonical_device_events_sha256'],b['canonical_device_events_sha256'])

    def test_actual_debug_pause_and_pending_dll_fresh_engine_reset(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'pause',PROFILE,scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=1000,execution_mode='bounded-continue')
            def mem(address,data):
                self.assertEqual(s.client.command(f'M{address:x},{len(data):x}:'+data.hex()),'OK')
            def word(address,value):mem(address,value.to_bytes(4,'little'))
            def count():
                return sum(e.get('event')=='device-transition' and e.get('device')==PHY_MODEL for e in
                    [json.loads(l) for l in (s.segment_dir/'engine-events.jsonl').read_text().splitlines()])
            try:
                mem(0,bytes.fromhex('0000a0e1')*16+bytes.fromhex('feffffea'))
                word(0xfffd8000,0x800c9);word(0xfffd80b4,0);word(0xfffd80b8,30);word(0xfffd80b4,0x40000000)
                self.assertEqual(s.client.command('Z0,4,4'),'OK')
                self.assertEqual(s.client.continue_run().signal,5)
                self.assertEqual(count(),0)
                before=(s.segment_dir/'engine-events.jsonl').read_bytes();time.sleep(0.02)
                self.assertEqual((s.segment_dir/'engine-events.jsonl').read_bytes(),before)
                self.assertEqual(s.client.read_memory(0xfffd80bc,4),bytes(4))
                self.assertEqual(s.client.command('z0,4,4'),'OK')
                self.assertEqual(s.client.command('Z0,20,4'),'OK')
                self.assertEqual(s.client.continue_run().signal,5)
                self.assertEqual(count(),1)
                self.assertEqual(int.from_bytes(s.client.read_memory(0xfffd80bc,4),'little'),0x8000)
                word(0xfffd80b4,0xc01f0000)
                self.assertEqual(s.client.command('z0,20,4'),'OK')
                self.assertEqual(s.client.command('Z0,24,4'),'OK')
                self.assertEqual(s.client.continue_run().signal,5)
                self.assertEqual(count(),1)
                s.reset();self.assertEqual(count(),0)
                self.assertEqual(s.client.read_memory(0xfffd80bc,4),bytes(4))
                s.continue_fast();self.assertEqual(count(),0)
            finally:s.close()


if __name__=='__main__':unittest.main()
