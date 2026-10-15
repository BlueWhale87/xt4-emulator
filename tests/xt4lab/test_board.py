"""Exercise policy at the real engine boundary, without physical devices."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from xt4lab.firmware import prepare_run

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
QEMU = REPO / 'build/qemu-system-arm'


class BoardTests(unittest.TestCase):
    def test_machine_available(self):
        result = subprocess.run([str(QEMU), '-machine', 'help'], capture_output=True,
                                text=True, timeout=3, check=True)
        self.assertIn('fujifilm-xt4', result.stdout)

    def test_xt4_rejects_extra_hardware(self):
        cases = [ ['-smp', '2'], ['-m', '1G'], ['-semihosting'], ['-usb'],
                  ['-device', 'usb-host'], ['-nic', 'model=unavailable'],
                  ['-kernel', str(RESEARCH / 'unpacked/segment-260274.bin')],
                  ['-bios', str(RESEARCH / 'unpacked/segment-260274.bin')] ]
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            root = Path(tmp)
            disk = root / 'unused.raw'
            disk.write_bytes(bytes(512))
            cases.append(['-drive', 'file=' + str(disk) + ',format=raw,if=none'])
            for index, args in enumerate(cases):
                with self.subTest(args=args):
                    run = prepare_run(RESEARCH, root / str(index), 'module-entry-diagnostic')
                    try:
                        result = subprocess.run([str(QEMU), '-machine',
                            'fujifilm-xt4,run-dir=' + str(run.directory), '-display', 'none',
                            '-nodefaults', '-S', *args], capture_output=True, timeout=2)
                    except subprocess.TimeoutExpired:
                        self.fail('Engine silently accepted unsupported override: ' + repr(args))
                    self.assertNotEqual(result.returncode, 0)

    def test_engine_revalidates_profile_and_bytes(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            root = Path(tmp)
            for what in ['name', 'overlap', 'register', 'image']:
                with self.subTest(what=what):
                    run = prepare_run(RESEARCH, root / what, 'module-entry-diagnostic')
                    p = run.directory / 'profile.json'
                    data = json.loads(p.read_text())
                    if what == 'name':
                        data['name'] = 'cold-boot'
                    elif what == 'overlap':
                        data['regions'][0]['base'] = 0x01e0f000
                    elif what == 'register':
                        data['registers']['sp'] = 0
                    else:
                        image = run.directory / 'inputs/primary.bin'
                        image.chmod(0o600)
                        image.write_bytes(b'broken')
                    p.write_text(json.dumps(data))
                    result = subprocess.run([str(QEMU), '-machine',
                        'fujifilm-xt4,run-dir=' + str(run.directory), '-display','none',
                        '-nodefaults','-S'],capture_output=True,timeout=2)
                    self.assertNotEqual(result.returncode,0)
                    self.assertFalse((run.directory / 'engine-events.jsonl').exists())

    def test_bootstrap_revalidation(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            for what in ['register', 'entry', 'size', 'image']:
                with self.subTest(what=what):
                    run = prepare_run(RESEARCH, Path(tmp) / what, 'bootstrap-entry-diagnostic')
                    p = run.directory / 'profile.json'
                    data = json.loads(p.read_text())
                    if what == 'register':
                        data['registers']['sp'] = 0x80000000
                    elif what == 'entry':
                        data['entry'] = 0x100
                    elif what == 'size':
                        data['regions'][0]['size'] = 0x40000
                    else:
                        image = run.directory / 'inputs/bootstrap.bin'
                        image.chmod(0o600)
                        image.write_bytes(b'broken')
                    p.write_text(json.dumps(data))
                    result = subprocess.run([str(QEMU), '-machine',
                        'fujifilm-xt4,run-dir=' + str(run.directory), '-display', 'none',
                        '-nodefaults', '-S'], capture_output=True, timeout=2)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((run.directory / 'engine-events.jsonl').exists())

    def test_bootstrap_cli_profile(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            path = Path(tmp) / 'cli'
            result = subprocess.run([sys.executable, '-m', 'xt4lab', 'run',
                '--research-root', str(RESEARCH), '--out', str(path), '--profile',
                'bootstrap-entry-diagnostic', '--steps', '1'], cwd=REPO,
                text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads((path / 'summary.json').read_text())
            self.assertEqual(data['completed_pcs'], [0])
            start = data['starting_states'][0]['registers']
            self.assertEqual((start['r15'], start['r13']), (0, 0))

    def test_engine_rejects_model_disagreement(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            cases = [('bootstrap-entry-diagnostic', ['xt4-port-config-v1']),
                     ('bootstrap-port-config-diagnostic', []),
                     ('bootstrap-port-config-diagnostic', ['unknown-model'])]
            for index, (name, models) in enumerate(cases):
                with self.subTest(name=name, models=models):
                    run = prepare_run(RESEARCH, Path(tmp) / str(index), name)
                    p = run.directory / 'profile.json'
                    data = json.loads(p.read_text())
                    data['models'] = models
                    p.write_text(json.dumps(data))
                    try:
                        result = subprocess.run([str(QEMU), '-machine',
                            'fujifilm-xt4,run-dir=' + str(run.directory), '-display', 'none',
                            '-nodefaults', '-S'], capture_output=True, timeout=2)
                    except subprocess.TimeoutExpired:
                        self.fail('C loader accepted a mismatched device model')
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((run.directory / 'engine-events.jsonl').exists())

if __name__ == '__main__':
    unittest.main()
