"""Strict covered-media identity and original-code calibration continuation."""
from dataclasses import fields, replace
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from xt4lab.firmware import load_profile, prepare_run, profile_dict, validate_profile, verify_prepared
from xt4lab.scenarios import scenario_dict, scenario_from_dict, scenario_sha256
from xt4lab.startup import CALIBRATION_SHA256, extract_calibration
from xt4lab.run import RunSession
from xt4lab.report import write_report

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
PROFILE = 'bootstrap-calibration-exploration'
ROUTE = 'xt4-request-route-exploration-v1'
DMA = 'xt4-dma-channel1-exploration-v1'
INDIRECT = 'xt4-indirect-read-exploration-v1'
MEDIA = 'xt4-covered-nor-media-v1'


class CalibrationAdmissionTests(unittest.TestCase):
    def test_previous_phy_identities_stay_exact(self):
        expected = [
            ('zero-ports-v1', 'b425b11783bc79b97f27793ba115c28cd1d7004eaee3d10da1ad3fbb2b183b15',
             'fc4f2def33fd5a8cbac7c24c389af3a1d1b90ffac20cc660a0e742a0e406d638'),
            ('one-ports-v1', '9dc6ddf407a0130a9765b6c8199dcff2f16d3a78f12cd659327d30d2ba36b386',
             '2e2598a46aeb52de109796ebb521687c3697ab0cd60ffb5cf5c4bc1dc89d1997')]
        for name, shash, phash in expected:
            p = load_profile('bootstrap-phy-exploration', scenario_name=name)
            self.assertEqual(scenario_sha256(p.scenario), shash)
            self.assertEqual(hashlib.sha256(json.dumps(profile_dict(p), sort_keys=True).encode()).hexdigest(), phash)
            self.assertNotIn('transfer', scenario_dict(p.scenario))
            self.assertNotIn('media', profile_dict(p))

    def test_exact_typed_transfer_and_media(self):
        with self.assertRaises(ValueError): load_profile(PROFILE)
        for name in ('zero-ports-v1', 'one-ports-v1'):
            p = load_profile(PROFILE, scenario_name=name)
            self.assertEqual(p.models[-4:], (ROUTE, DMA, INDIRECT, MEDIA))
            self.assertEqual((p.media.file_size, p.media.sha256, p.media.flash_offset,
                              p.media.capacity, p.media.virtual_base),
                             (128, CALIBRATION_SHA256, 0x2a080, 0x4000000, 0xf0000000))
            self.assertEqual(p.scenario.transfer.cbar_base, 0xf8f00000)
            plain = scenario_dict(p.scenario)
            self.assertEqual(scenario_from_dict(plain, timer=True, platform=True, flash=True, phy=True, transfer=True), p.scenario)
            for state, attr in ((p.media, 'media'), (p.scenario.transfer, 'transfer')):
                for f in fields(state):
                    for bad in (None, False, 1.0, 'other'):
                        changed = replace(state, **{f.name: bad})
                        candidate = replace(p, media=changed) if attr == 'media' else replace(p, scenario=replace(p.scenario, transfer=changed))
                        with self.subTest(field=f.name, bad=bad), self.assertRaises(ValueError):
                            validate_profile(candidate)
            for old_name in ('bootstrap-phy-exploration', 'bootstrap-flash-exploration', 'bootstrap-controller-exploration'):
                old = load_profile(old_name, scenario_name=name)
                with self.assertRaises(ValueError): validate_profile(replace(old, media=p.media))
                with self.assertRaises(ValueError): validate_profile(replace(old, scenario=replace(old.scenario, transfer=p.scenario.transfer)))
                for bad in (None, plain['transfer']):
                    with self.assertRaises(ValueError): scenario_from_dict({**scenario_dict(old.scenario), 'transfer': bad}, timer=True, platform=True, flash=old.scenario.flash is not None, phy=old.scenario.phy is not None)
            for f in fields(p.scenario.transfer):
                d = json.loads(json.dumps(plain)); d['transfer'].pop(f.name)
                with self.assertRaises(ValueError): scenario_from_dict(d, timer=True, platform=True, flash=True, phy=True, transfer=True)

    def test_staged_extent_and_every_segment_reset(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp) / 'run', PROFILE, scenario_name='zero-ports-v1')
            expected = extract_calibration((RESEARCH / 'FWUP0024.DAT').read_bytes())
            self.assertEqual((run.directory / run.profile.media.file).read_bytes(), expected)
            self.assertEqual((run.directory / run.profile.media.file).stat().st_mode & 0o777, 0o400)
            s = RunSession(run, REPO / 'build/qemu-system-arm', budget=1, execution_mode='bounded-continue')
            try:
                for segment in range(2):
                    self.assertEqual((s.segment_dir / run.profile.media.file).read_bytes(), expected)
                    self.assertEqual((s.segment_dir / run.profile.media.file).stat().st_mode & 0o777, 0o400)
                    self.assertEqual(s.client.read_memory(0xca00, 128), bytes(128))
                    self.assertEqual(s.continue_fast()['reason'], 'instruction_budget')
                    if segment == 0: s.reset()
                self.assertIn('-nic', s.argv); self.assertIn('none', s.argv)
            finally: s.close()
            write_report(run.directory)
            path = run.directory / run.profile.media.file
            path.chmod(0o600); path.write_bytes(bytes(128))
            with self.assertRaisesRegex(ValueError, 'media'): verify_prepared(run)

    def test_c_independent_metadata_and_bytes_admission(self):
        p = load_profile(PROFILE, scenario_name='zero-ports-v1')
        base = profile_dict(p)
        mutations = []
        for section in ('media', 'transfer'):
            target = base['media'] if section == 'media' else base['scenario']['transfer']
            for key, value in target.items():
                mutations += [(section, key, bad) for bad in (None, False, float(value) if type(value) is int else 'other')]
            mutations.append((section, 'extra', 0))
        mutations += [('media', 'whole', None), ('transfer', 'whole', None), ('bytes', 'short', None), ('bytes', 'changed', None), ('bytes', 'symlink', None), ('old', 'null', None)]
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            for i, (section, key, value) in enumerate(mutations):
                run = prepare_run(RESEARCH, Path(tmp) / str(i), PROFILE, scenario_name='zero-ports-v1')
                d = profile_dict(p)
                if section == 'bytes':
                    file = run.directory / p.media.file; file.chmod(0o600)
                    if key == 'symlink': file.unlink(); file.symlink_to('bootstrap.bin')
                    else: file.write_bytes(bytes(127 if key == 'short' else 128))
                elif section == 'old':
                    d = profile_dict(load_profile('bootstrap-phy-exploration', scenario_name='zero-ports-v1')); d['media'] = None
                else:
                    target = d if section == 'media' else d['scenario']
                    if key == 'whole': target.pop(section)
                    elif value is None: target[section].pop(key)
                    else: target[section][key] = value
                (run.directory / 'profile.json').write_text(json.dumps(d))
                r = subprocess.run([str(REPO / 'build/qemu-system-arm'), '-machine', 'fujifilm-xt4,run-dir=' + str(run.directory), '-S', '-nodefaults', '-display', 'none', '-accel', 'tcg,thread=single', '-icount', 'shift=0,align=off,sleep=off'], capture_output=True, timeout=2)
                with self.subTest(section=section, key=key, value=value):
                    self.assertNotEqual(r.returncode, 0)
                    self.assertFalse((run.directory / 'engine-events.jsonl').exists())

    def test_report_binds_media_cbar_and_both_profile_directions(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            for i, actual in enumerate((PROFILE, 'bootstrap-phy-exploration')):
                run = prepare_run(RESEARCH, Path(tmp) / str(i), actual, scenario_name='zero-ports-v1')
                s = RunSession(run, REPO / 'build/qemu-system-arm', budget=1, execution_mode='bounded-continue')
                try: self.assertEqual(s.continue_fast()['reason'], 'instruction_budget')
                finally: s.close()
                write_report(run.directory)
                path = run.directory / 'run-info.json'; original = json.loads(path.read_text())
                other = load_profile('bootstrap-phy-exploration' if actual == PROFILE else PROFILE, scenario_name='zero-ports-v1')
                d = json.loads(json.dumps(original)); d['profile'] = profile_dict(other); d['provenance']['scenario_sha256'] = scenario_sha256(other.scenario)
                path.write_text(json.dumps(d))
                with self.assertRaisesRegex(ValueError, 'engine'): write_report(run.directory)
                if actual == PROFILE:
                    for section, key, value in [('media', 'sha256', '0' * 64), ('media', 'whole', None), ('transfer', 'cbar_base', 0), ('transfer', 'sample_policy', 'other')]:
                        d = json.loads(json.dumps(original))
                        if section == 'media':
                            if key == 'whole': d['profile'].pop('media')
                            else: d['profile']['media'][key] = value
                        else: d['profile']['scenario']['transfer'][key] = value
                        path.write_text(json.dumps(d))
                        with self.assertRaises(ValueError): write_report(run.directory)


class CalibrationRunTests(unittest.TestCase):
    def events(self, s):
        return [json.loads(l) for l in (s.segment_dir / 'engine-events.jsonl').read_text().splitlines()]

    def completions(self, s):
        return [e for e in self.events(s) if e.get('event') == 'device-transition' and e.get('device') == INDIRECT]

    def mem(self, s, address, data):
        self.assertEqual(s.client.command(f'M{address:x},{len(data):x}:' + data.hex()), 'OK')

    def word(self, s, address, value):
        self.mem(s, address, value.to_bytes(4, 'little'))

    def advance(self, s, count):
        address = count * 4
        self.assertEqual(s.client.command(f'Z0,{address:x},4'), 'OK')
        self.assertEqual(s.client.continue_run().signal, 5)
        self.assertEqual(s.client.command(f'z0,{address:x},4'), 'OK')
        self.assertEqual(s._execution_snapshot()['engine_instruction_count'], count)

    def pending(self, s):
        # Anonymous NOPs make actual CPU/virtual time deterministic; peripheral
        # timers, MMIO admission and RAM delivery remain the production devices.
        self.mem(s, 0, bytes.fromhex('0000a0e1') * 1024)
        self.word(s, 0xfffd8000, 0x800c1)
        self.word(s, 0xfffd8090, 0x06000001)
        self.advance(s, 68)
        self.word(s, 0xfffd8094, 0)
        self.word(s, 0xfffd80a8, 2)
        self.word(s, 0xfffd8090, 0x720b8001)
        self.advance(s, 176)
        for address, value in [(0xfffd8000,0xc10080c9),(0xfffd8004,0x140333ee),
            (0xfffd8010,1),(0xfffd8014,0x101003),(0xfffd801c,0xf002a080),
            (0xfffd8020,0x707),(0xfffd8080,0xf),(0xfffd80e0,0x11edfa00),
            (0xfffd80f0,0x10),(0xfffd80b4,0),(0xfffd80b8,30),(0xfffd80b4,0x40000000)]:
            self.word(s, address, value)
        self.advance(s, 184)
        self.word(s, 0xfffd80b4, 0xc01f0000)
        self.advance(s, 192)
        for address, value in [(0xfff4a500,0x01002a2c),(0xfffe1000,0x10000000),
            (0xfffe1028,0x00201011),(0xfffe1030,0),(0xfffe1010,127),
            (0xfffe1014,0xf002a080),(0xfffe1018,0x10000),(0xfffe101c,0x30f04),
            (0xfffe1020,0x30f00),(0xfffe1024,0),(0xfffe1028,0x10201011),
            (0xfffd8064,0),(0xfffd8068,0xf002a080),(0xfffd806c,128)]:
            self.word(s, address, value)
        self.mem(s, 0x10000, b'\xa5' * 128)
        self.word(s, 0xfffd8060, 1)
        self.assertEqual(int.from_bytes(s.client.read_memory(0xfffd8060,4),'little'),1)
        self.assertEqual(int.from_bytes(s.client.read_memory(0xfffe1030,4),'little'),0x10000)
        self.assertEqual(s.client.read_memory(0x10000,128), b'\xa5' * 128)
        self.assertEqual(self.completions(s), [])
        self.assertFalse([e for e in self.events(s) if e.get('success') is False])

    def test_pending_pause_resume_and_full_delivery(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'pause',PROFILE,scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=512,execution_mode='bounded-continue')
            try:
                self.pending(s)
                before=(s.segment_dir/'engine-events.jsonl').read_bytes()
                time.sleep(.02)
                self.assertEqual((s.segment_dir/'engine-events.jsonl').read_bytes(),before)
                self.assertEqual(s.client.read_memory(0x10000,128), b'\xa5' * 128)
                self.advance(s,288)
                self.assertEqual(s.client.read_memory(0x10000,128),(run.directory/'inputs/calibration.bin').read_bytes())
                self.assertEqual([(e['started_ns'],e['virtual_ns']) for e in self.completions(s)],[(192,256)])
            finally:s.close()

    def test_pending_budget_tie_and_earlier_stop_cancel_delivery(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            for budget in (224,256):
                run=prepare_run(RESEARCH,Path(tmp)/str(budget),PROFILE,scenario_name='zero-ports-v1')
                s=RunSession(run,REPO/'build/qemu-system-arm',budget=budget,execution_mode='bounded-continue')
                try:
                    self.pending(s)
                    result=s.continue_fast()
                    self.assertEqual((result['reason'],result['engine_instruction_count']),('instruction_budget',budget))
                    self.assertEqual(s.client.read_memory(0x10000,128),b'\xa5'*128)
                    self.assertEqual(self.completions(s),[])
                    before=(s.segment_dir/'engine-events.jsonl').read_bytes()
                    self.assertEqual(s.continue_fast(),result)
                    self.assertEqual((s.segment_dir/'engine-events.jsonl').read_bytes(),before)
                    s.reset()
                    self.assertEqual(s.client.read_memory(0x10000,128),bytes(128))
                    self.assertEqual(s.continue_fast()['reason'],'instruction_budget')
                    self.assertEqual(self.completions(s),[])
                finally:s.close()
                write_report(run.directory)

    def test_pending_lost_global_even_when_reenabled(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'global',PROFILE,scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=512,execution_mode='bounded-continue')
            try:
                self.pending(s)
                self.word(s,0xfffe1000,0)
                self.word(s,0xfffe1000,0x10000000)
                result=s.continue_fast()
                self.assertEqual(result['reason'],'unsupported_device_transition')
                self.assertEqual(result['detail'],'changed_calibration_prerequisite')
                self.assertIsNone(result['width']);self.assertIsNone(result['value'])
                self.assertEqual(s.client.read_memory(0x10000,128),b'\xa5'*128)
                self.assertFalse([e for e in self.completions(s) if e['success']])
            finally:s.close()

    def test_pending_real_wall_timeout_reaps_and_fresh_reset(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            run=prepare_run(RESEARCH,Path(tmp)/'timeout',PROFILE,scenario_name='zero-ports-v1')
            s=RunSession(run,REPO/'build/qemu-system-arm',budget=1000,wall_limit=1,execution_mode='bounded-continue')
            try:
                self.pending(s)
                process=s.process
                before=(s.segment_dir/'engine-events.jsonl').read_bytes()
                time.sleep(max(0,s.client.deadline-time.monotonic())+.01)
                result=s.continue_fast()
                self.assertEqual(result['reason'],'timeout')
                self.assertIsNotNone(process.poll())
                self.assertEqual((s.segment_dir/'engine-events.jsonl').read_bytes(),before)
                self.assertEqual(s.continue_fast(),result)
                s.reset()
                self.assertEqual(s.client.read_memory(0x10000,128),bytes(128))
                self.assertEqual(s.continue_fast()['reason'],'instruction_budget')
                self.assertEqual(self.completions(s),[])
            finally:s.close()

    def original(self, root, name, scenario, reset=False):
        run=prepare_run(RESEARCH,root/name,PROFILE,scenario_name=scenario)
        expected=(run.directory/'inputs/calibration.bin').read_bytes()
        s=RunSession(run,REPO/'build/qemu-system-arm',budget=200000,execution_mode='bounded-continue')
        try:
            for segment in range(2 if reset else 1):
                result=s.continue_fast()
                self.assertEqual((result['reason'],result['pc'],result['physical_address'],result['width']),
                                 ('unsupported_access',0x34b0,0xf8f01000,4))
                self.assertEqual(result['engine_instruction_count'],96742)
                self.assertEqual(s.client.read_memory(0xca00,128),expected)
                self.assertEqual(s.client.read_memory(0x10acc,32),bytes.fromhex(
                    '010000003e0000000000000007010000c2000000bcc100000100000080a002f0'))
                done=self.completions(s)
                self.assertEqual([e['phase'] for e in done],[20]*32+[21]*32)
                for e in done:
                    self.assertTrue(e['success']);self.assertTrue(e['done']);self.assertFalse(e['active'])
                    self.assertEqual((e['source'],e['destination'],e['source_length']), (0xf002a080,0xca00,128))
                    self.assertEqual(e['source_sha256'],CALIBRATION_SHA256)
                    self.assertEqual(e['virtual_ns']-e['started_ns'],64)
                writes=[(e['address'],e['value']) for e in self.events(s) if e.get('event')=='device-access'
                        and e.get('operation')=='write' and e.get('device') in (ROUTE,DMA,INDIRECT)]
                # 3a58 writes disable twice before source/destination/count;
                # 3764 then acknowledges status and disables once more.
                cycle=[(0xfff4a500,0x01002a2c),(0xfffe1028,0x00201011),(0xfffe1028,0x00201011),
                    (0xfffe1014,0xf002a080),(0xfffe1018,0xca00),(0xfffe1010,127),
                    (0xfffe101c,0x30f04),(0xfffe1020,0x30f00),(0xfffe1024,0),
                    (0xfffe1030,0),(0xfffe1028,0x00201011),(0xfffe1028,0x10201011),
                    (0xfffd8064,0),(0xfffd8068,0xf002a080),(0xfffd806c,128),
                    (0xfffd8060,1),(0xfffd8060,0x20),(0xfffe1030,0),
                    (0xfffe1028,0x00201011),(0xfff4a500,0x01002c2c)]
                self.assertEqual(len(writes),len(cycle)*64)
                for i in range(64):self.assertEqual(writes[i*len(cycle):(i+1)*len(cycle)],cycle)
                if reset and segment==0:s.reset()
        finally:s.close()
        write_report(run.directory)
        summary=json.loads((run.directory/'summary.json').read_text())
        self.assertEqual((summary['modeled_write_count'],summary['modeled_read_count']),(2380,2304))
        self.assertFalse(summary['camera_booted'])
        return summary

    def test_original_all_transfers_both_fixtures_repeat_and_reset(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            a=self.original(Path(tmp),'zero','zero-ports-v1',reset=True)
            b=self.original(Path(tmp),'one','one-ports-v1')
            c=self.original(Path(tmp),'repeat','zero-ports-v1')
            self.assertEqual(a['canonical_device_events_sha256'],c['canonical_device_events_sha256'])
            self.assertEqual(a['engine_instruction_count'],b['engine_instruction_count'])


if __name__ == '__main__': unittest.main()
