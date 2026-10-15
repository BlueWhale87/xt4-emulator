"""Execute the original alternate flash branch without inventing a chip/window."""
from dataclasses import fields, replace
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from xt4lab.firmware import prepare_run, load_profile, profile_dict, validate_profile
from xt4lab.run import RunSession
from xt4lab.report import write_report

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
PROFILE = 'bootstrap-selector2c-exploration'
PART = 'selector2c-spi-boot-v1'
INDIRECT = 'xt4-indirect-read-exploration-v1'


class Selector2cTests(unittest.TestCase):
    def profile(self, name=PROFILE):
        try:
            return load_profile(name, scenario_name='zero-ports-v1')
        except ValueError as exc:
            self.fail(str(exc))

    def events(self, session):
        return [json.loads(l) for l in (session.segment_dir/'engine-events.jsonl').read_text().splitlines()]

    def test_profile_contract_and_cross_part_rejection(self):
        with self.assertRaises(ValueError): load_profile(PROFILE)
        p = self.profile()
        self.assertIn(PART, p.models)
        self.assertNotIn('mx25um51245gxdi00-boot-v1', p.models)
        self.assertEqual(p.scenario.flash.virtual_part, PART)
        self.assertEqual(p.scenario.flash.cr2_typical_settle_ns, 0)
        self.assertEqual(p.scenario.transfer.sample_policy, 'ideal-taps0-127-phases16-17-v1')
        old = self.profile('bootstrap-calibration-exploration')
        for target, donor in ((p, old), (old, p)):
            with self.assertRaises(ValueError):
                validate_profile(replace(target, scenario=donor.scenario))
        for f in fields(p.scenario.flash):
            for bad in (None, False, 1.0, 'unknown'):
                candidate = replace(p, scenario=replace(p.scenario, flash=replace(p.scenario.flash, **{f.name:bad})))
                with self.subTest(field=f.name, bad=bad), self.assertRaises(ValueError):
                    validate_profile(candidate)

    def test_engine_independently_rejects_cross_part_metadata(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for n, (target, donor, what) in enumerate([
                (PROFILE,'bootstrap-calibration-exploration','scenario'),
                ('bootstrap-calibration-exploration',PROFILE,'scenario'),
                (PROFILE,'bootstrap-calibration-exploration','models'),
                (PROFILE,PROFILE,'type')]):
                run=prepare_run(RESEARCH,Path(tmp)/str(n),target,scenario_name='zero-ports-v1')
                d=profile_dict(run.profile)
                other=profile_dict(self.profile(donor))
                if what=='type': d['scenario']['flash']['cr2_typical_settle_ns']=False
                else: d[what]=other[what]
                (run.directory/'profile.json').write_text(json.dumps(d))
                result=subprocess.run([str(REPO/'build/qemu-system-arm'),'-machine',
                    'fujifilm-xt4,run-dir='+str(run.directory),'-display','none',
                    '-nodefaults','-nic','none','-S','-icount','shift=0'],capture_output=True,timeout=3)
                self.assertNotEqual(result.returncode,0)
                self.assertFalse((run.directory/'engine-events.jsonl').exists())

    def original(self, directory, scenario, reset=False):
        run=prepare_run(RESEARCH,directory,PROFILE,scenario_name=scenario)
        session=RunSession(run,REPO/'build/qemu-system-arm',budget=200000,execution_mode='bounded-continue')
        results=[]
        try:
            for segment in range(2 if reset else 1):
                stop=session.continue_fast()
                self.assertEqual((stop['reason'],stop['pc'],stop['physical_address'],stop['width']),
                                 ('unsupported_access',0x34b0,0xf8f01000,4))
                self.assertEqual(session.client.read_memory(0xca00,128),(run.directory/'inputs/calibration.bin').read_bytes())
                self.assertEqual(struct.unpack('<6I',session.client.read_memory(0x10acc,24)),(1,62,0,0x107,0x2c,0xc504))
                events=self.events(session)
                done=[e for e in events if e.get('event')=='device-transition' and e.get('device')==INDIRECT]
                self.assertEqual([e['phase'] for e in done],[16]*32+[17]*32)
                self.assertTrue(all(e['success'] and e['done'] and e['source_length']==128 for e in done))
                nor=[e for e in events if e.get('operation')=='nor-complete']
                self.assertEqual([(e['opcode'],e['protocol'],e['virtual_part']) for e in nor],[(0x9f,'spi',PART)])
                self.assertFalse([e for e in events if e.get('success') is False and e.get('device')])
                results.append(stop['engine_instruction_count'])
                if reset and segment==0: session.reset()
        finally: session.close()
        report=write_report(run.directory).read_text()
        self.assertIn(PART,report)
        self.assertIn('phases16/17',report)
        self.assertNotIn('named virtual SPI-start MX25',report)
        # Report cannot accept the other profile's otherwise valid declaration.
        info_path=run.directory/'run-info.json'
        info=json.loads(info_path.read_text())
        info['profile']['scenario']=profile_dict(self.profile('bootstrap-calibration-exploration'))['scenario']
        info_path.write_text(json.dumps(info))
        with self.assertRaises(ValueError): write_report(run.directory)
        return results

    def test_original_both_fixtures_repeat_and_reset(self):
        self.profile()
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            root=Path(tmp)
            counts=self.original(root/'zero','zero-ports-v1')
            self.assertEqual(self.original(root/'one','one-ports-v1'),counts)
            self.assertEqual(self.original(root/'repeat','zero-ports-v1'),counts)
            self.assertEqual(self.original(root/'reset','zero-ports-v1',True),counts*2)

    def test_mixed_array_tuple_rejects_before_delivery(self):
        self.profile()
        # Break caught: admitting the old part's read/size/config tuple for the
        # alternate part must never produce a successful memory transfer.
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for n, changed in enumerate([(0xfffd8004,0x140333ee),
                                         (0xfffd8014,0x101003),
                                         (0xfffd8000,0x410080c9)]):
                run=prepare_run(RESEARCH,Path(tmp)/str(n),PROFILE,scenario_name='zero-ports-v1')
                s=RunSession(run,REPO/'build/qemu-system-arm',budget=512,execution_mode='bounded-continue')
                def mem(address,data):
                    self.assertEqual(s.client.command(f'M{address:x},{len(data):x}:'+data.hex()),'OK')
                def word(address,value): mem(address,value.to_bytes(4,'little'))
                def advance(count):
                    address=count*4
                    self.assertEqual(s.client.command(f'Z0,{address:x},4'),'OK')
                    self.assertEqual(s.client.continue_run().signal,5)
                    self.assertEqual(s.client.command(f'z0,{address:x},4'),'OK')
                try:
                    mem(0,bytes.fromhex('0000a0e1')*1024)
                    for a,v in [(0xfffd8000,0x80c9),(0xfffd8004,0x100334fd),
                        (0xfffd8010,1),(0xfffd8014,0x111003),(0xfffd801c,0xf002a080),
                        (0xfffd8020,0x707),(0xfffd8080,0xf),(0xfffd80e0,0xfa00),
                        (0xfffd80f0,0),(0xfffd80b4,0),(0xfffd80b8,30),(0xfffd80b4,0x40000000)]:
                        word(a,v)
                    advance(8); word(0xfffd80b4,0xc01f0000); advance(16)
                    for a,v in [(0xfff4a500,0x01002a2c),(0xfffe1000,0x10000000),
                        (0xfffe1028,0x00201011),(0xfffe1030,0),(0xfffe1010,127),
                        (0xfffe1014,0xf002a080),(0xfffe1018,0x10000),(0xfffe101c,0x30f04),
                        (0xfffe1020,0x30f00),(0xfffe1024,0),(0xfffe1028,0x10201011),
                        (0xfffd8064,0),(0xfffd8068,0xf002a080),(0xfffd806c,128)]:
                        word(a,v)
                    mem(0x10000,b'\xa5'*128)
                    word(*changed)
                    self.assertEqual(s.client.command('Mfffd8060,4:01000000'),'E14')
                    rejected=[e for e in self.events(s) if e.get('success') is False]
                    self.assertEqual(rejected[-1]['detail'],'unsupported_calibration_flash_state')
                    advance(256)
                    self.assertEqual(s.client.read_memory(0x10000,128),b'\xa5'*128)
                    self.assertFalse([e for e in self.events(s) if e.get('operation')=='transfer-complete'])
                finally: s.close()
