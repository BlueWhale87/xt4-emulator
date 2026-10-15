"""Strict GIC adapter, real architectural IRQ entry and original continuation."""
from dataclasses import replace
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from xt4lab.firmware import prepare_run, load_profile, profile_dict, validate_profile
from xt4lab.run import RunSession
from xt4lab.report import write_report

REPO=Path(__file__).resolve().parents[2]
RESEARCH=REPO.parents[1]/'research/2026-10-14-xt4-firmware'
PROFILE='bootstrap-gic-exploration'
GIC='xt4-gicv2-core0-exploration-v1'
D=0xf8f01000
C=0xf8f02000


class GicTests(unittest.TestCase):
    def profile(self):
        try:return load_profile(PROFILE,scenario_name='zero-ports-v1')
        except ValueError as exc:self.fail(str(exc))

    def session(self,path,scenario='zero-ports-v1',budget=256):
        run=prepare_run(RESEARCH,path,PROFILE,scenario_name=scenario)
        return RunSession(run,REPO/'build/qemu-system-arm',budget=budget,execution_mode='bounded-continue')

    def mem(self,s,address,data):
        self.assertEqual(s.client.command(f'M{address:x},{len(data):x}:'+data.hex()),'OK')

    def word(self,s,address,value):self.mem(s,address,value.to_bytes(4,'little'))

    def read(self,s,address):return int.from_bytes(s.client.read_memory(address,4),'little')

    def events(self,s):return [json.loads(l) for l in (s.segment_dir/'engine-events.jsonl').read_text().splitlines()]

    def enable(self,s):
        self.word(s,D,1);self.word(s,C+4,255);self.word(s,C,1)

    def test_exact_contract_and_independent_c_admission(self):
        p=self.profile()
        self.assertIn(GIC,p.models)
        self.assertEqual((p.scenario.gic.cpu_count,p.scenario.gic.irq_count,p.scenario.gic.priority_bits),(1,64,5))
        old=load_profile('bootstrap-selector2c-exploration',scenario_name='zero-ports-v1')
        for target,donor in ((p,old),(old,p)):
            with self.assertRaises(ValueError):validate_profile(replace(target,scenario=donor.scenario))
        base=profile_dict(p)
        mutations=[]
        for key,value in base['scenario']['gic'].items():
            mutations.extend((key,bad) for bad in (None,1.0,'unknown'))
        mutations.extend([('extra',0),('whole',None)])
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for n,(key,bad) in enumerate(mutations):
                run=prepare_run(RESEARCH,Path(tmp)/str(n),PROFILE,scenario_name='zero-ports-v1')
                d=profile_dict(p)
                if key=='whole':d['scenario']['gic']=bad
                else:d['scenario']['gic'][key]=bad
                (run.directory/'profile.json').write_text(json.dumps(d))
                result=subprocess.run([str(REPO/'build/qemu-system-arm'),'-machine',
                    'fujifilm-xt4,run-dir='+str(run.directory),'-display','none','-nodefaults',
                    '-nic','none','-S','-icount','shift=0'],capture_output=True,timeout=3)
                self.assertNotEqual(result.returncode,0)
                self.assertFalse((run.directory/'engine-events.jsonl').exists())

    def test_sgi_ack_eoi_target_filter_and_reset(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            s=self.session(Path(tmp)/'run')
            try:
                self.assertEqual(self.read(s,D),0);self.assertEqual(self.read(s,C),0)
                self.assertEqual(self.read(s,C+0xc),1023)
                self.enable(s)
                self.word(s,D+0xf00,0x01000001)
                self.assertEqual(self.read(s,C+0xc),1023) # no other core exists
                self.word(s,D+0xf00,0x02000001)
                self.assertEqual(self.read(s,D+0x200)&2,2)
                self.assertEqual(self.read(s,C+0xc),1)
                self.assertEqual(self.read(s,D+0x300)&2,2)
                self.word(s,C+0x10,1)
                self.assertEqual(self.read(s,D+0x300)&2,0)
                self.assertEqual(self.read(s,C+0xc),1023)
                self.word(s,D+0xf00,0x02000001)
                s.reset()
                self.assertEqual(self.read(s,D),0);self.assertEqual(self.read(s,C),0)
                self.assertEqual(self.read(s,D+0x200)&2,0)
                self.assertEqual(self.read(s,D+0x300)&2,0)
                self.assertEqual(self.read(s,C+0xc),1023)
            finally:s.close()

    def test_invalid_accesses_do_not_mutate_prior_state(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            s=self.session(Path(tmp)/'run')
            try:
                self.word(s,C+4,0xa8)
                for a,data in [(C+4,b'\x00'),(C+4,(256).to_bytes(4,'little')),
                    (D,(2).to_bytes(4,'little')),(D+4,bytes(4)),
                    (D+0xf00,(0x03000001).to_bytes(4,'little')),(D+0xffc,bytes(4))]:
                    self.assertEqual(s.client.command(f'M{a:x},{len(data):x}:'+data.hex()),'E14')
                    self.assertEqual(self.read(s,C+4),0xa8)
                self.assertEqual(s.client.command(f'm{D+0xf00:x},4'),'E14')
                self.assertEqual(s.client.command(f'm{C+0x10:x},4'),'E14')
                self.assertFalse([e for e in self.events(s) if e.get('operation')=='irq-level' and e.get('irq_level')])
            finally:s.close()

    def test_guest_irq_vector_acknowledgement_and_architectural_return(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            s=self.session(Path(tmp)/'irq')
            try:
                # Anonymous test instructions: set IRQ stack, return SVC,
                # unmask IRQ, then write a mainline marker and loop.
                self.word(s,0,0xea00003e);self.word(s,0x18,0xea000078)
                main=[0xf1020012,0xe30bd000,0xe340d001,0xf1020013,0xf1080080,
                      0xe3000000,0xe3400001,0xe3a01055,0xe5801004,0xeafffffe]
                handler=[0xe92d400f,0xe302000c,0xe34f08f0,0xe5901000,
                         0xe3002000,0xe3402001,0xe5821000,0xe5801004,0xe8bd400f,0xe25ef004]
                self.mem(s,0x100,struct.pack('<10I',*main))
                self.mem(s,0x200,struct.pack('<10I',*handler))
                self.enable(s);self.word(s,D+0xf00,0x02000001)
                stop=s.continue_fast()
                self.assertEqual(stop['reason'],'instruction_budget')
                self.assertEqual(struct.unpack('<2I',s.client.read_memory(0x10000,8)),(1,0x55))
                self.assertEqual(self.read(s,D+0x300)&2,0)
                self.assertEqual(self.read(s,C+0xc),1023)
                self.assertEqual(s.continue_fast(),stop)
                s.reset()
                self.assertEqual(s.client.read_memory(0x10000,8),bytes(8))
            finally:s.close()

    def test_other_architectural_exceptions_remain_strict(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            s=self.session(Path(tmp)/'svc')
            try:
                self.word(s,0,0xef000000)
                self.assertEqual(s.continue_fast()['reason'],'unexpected_exception')
            finally:s.close()

    def test_original_gic_initialization_and_next_strict_boundary(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            counts=[]
            for n,scenario in enumerate(['zero-ports-v1','one-ports-v1','zero-ports-v1']):
                s=self.session(Path(tmp)/str(n),scenario,budget=250000)
                try:
                    for segment in range(2 if n==2 else 1):
                        stop=s.continue_fast()
                        self.assertEqual(stop['reason'],'unsupported_access')
                        self.assertEqual((stop['pc'],stop['physical_address'],stop['engine_instruction_count']),
                                         (0x4f34,0xfffd8060,96412))
                        events=self.events(s)
                        self.assertEqual([e['value'] for e in events if e.get('device')==GIC and e.get('operation')=='write' and e.get('address')==D+0xf00],
                                         [0x01000001,0x01000002,0x01000003])
                        self.assertEqual(len([e for e in events if e.get('operation')=='transfer-complete' and e.get('device')=='xt4-indirect-read-exploration-v1']),64)
                        counts.append((stop['pc'],stop['physical_address'],stop['engine_instruction_count']))
                        if n==2 and segment==0:s.reset()
                finally:s.close()
                report=write_report(s.prepared.directory).read_text()
                self.assertIn(GIC,report)
            self.assertTrue(all(c==counts[0] for c in counts))
