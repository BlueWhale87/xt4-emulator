from dataclasses import replace
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'


class FirmwareTests(unittest.TestCase):
    def setUp(self):
        (REPO / 'runs').mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=REPO / 'runs', prefix='test-input-')
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def fw(self):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.firmware'),
                             'firmware validation implementation missing')
        from xt4lab import firmware
        return firmware

    def fixture(self):
        root = self.root / 'source'
        (root / 'unpacked').mkdir(parents=True)
        for rel in ['FWUP0024.DAT', 'unpacked/segment-260274.bin',
                    'unpacked/segment-5c0274.bin', 'unpacked/segment-ae0274.bin']:
            shutil.copyfile(RESEARCH / rel, root / rel)
        return root

    def test_exact_pins(self):
        run = self.fw().prepare_run(RESEARCH, self.root / 'run with spaces',
                                    'module-entry-diagnostic')
        self.assertEqual(run.manifest['original_sha256'],
                         '853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5')
        self.assertEqual([r.sha256 for r in run.profile.regions if r.file], [
            '0b5b0fb0ee5fe6b109dae9964b5cddfe18edab46b4a0c195f9da194448906eeb',
            'e1ef8b30903d3078fde103cdd134fca7bfc4337d67bcaf55c3080ffd5fd93b13',
            '6c8d7b8b0a37df8c2b636c504c07a80318243b313d5bf435e2c39ef3f56e9f80'])
        self.assertEqual(run.profile.registers['sp'], 0x80000000)
        self.assertEqual(run.directory.stat().st_mode & 0o777, 0o700)

    def test_invalid_layout(self):
        f = self.fw()
        p = f.load_profile('module-entry-diagnostic')
        first = p.regions[0]
        for bad in [replace(first, base=0xffffffff), replace(first, size=1),
                    replace(first, base=p.regions[1].base)]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                f.validate_profile(replace(p, regions=(bad, *p.regions[1:])))
        with self.assertRaises(ValueError):
            f.validate_profile(replace(p, entry=0xe20008))

    def test_changed_source(self):
        f = self.fw()
        source = self.fixture()
        (source / 'unpacked/segment-260274.bin').write_bytes(b'bad')
        with self.assertRaises(ValueError):
            f.prepare_run(source, self.root / 'bad-run', 'module-entry-diagnostic')
        self.assertFalse((self.root / 'bad-run/profile.json').exists())

    def test_path_types(self):
        f = self.fw()
        with self.assertRaises(ValueError):
            f.read_regular(Path('/dev'), 'null')
        os.mkfifo(self.root / 'fifo')
        with self.assertRaises(ValueError):
            f.read_regular(self.root, 'fifo')
        (self.root / 'escape').symlink_to(RESEARCH / 'FWUP0024.DAT')
        for rel in ['escape', '../FWUP0024.DAT']:
            with self.subTest(rel=rel), self.assertRaises(ValueError):
                f.read_regular(self.root, rel)
        existing = self.root / 'exists'
        existing.mkdir()
        with self.assertRaises(ValueError):
            f.prepare_run(RESEARCH, existing, 'module-entry-diagnostic')
        outside = self.root / 'outside'
        outside.symlink_to(Path('/tmp').resolve(), target_is_directory=True)
        with self.assertRaises(ValueError):
            f.prepare_run(RESEARCH, outside / 'xt4-no-create', 'module-entry-diagnostic')

    def test_private_copy_is_revalidated(self):
        f = self.fw()
        source = self.fixture()
        run = f.prepare_run(source, self.root / 'run', 'module-entry-diagnostic')
        (source / 'unpacked/segment-260274.bin').write_bytes(b'changed later')
        f.verify_prepared(run)
        staged = run.directory / 'inputs/primary.bin'
        staged.chmod(0o600)
        staged.write_bytes(b'changed staged copy')
        with self.assertRaises(ValueError):
            f.verify_prepared(run)

    def test_validation_has_no_invented_state(self):
        with self.assertRaisesRegex(ValueError, 'missing_boot_state'):
            self.fw().prepare_run(RESEARCH, self.root / 'no-boot', 'validation')
        self.assertFalse((self.root / 'no-boot').exists())


if __name__ == '__main__':
    unittest.main()
