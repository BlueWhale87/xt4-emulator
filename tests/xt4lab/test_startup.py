import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from xt4lab.firmware import prepare_run, verify_prepared

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'


class StartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = (RESEARCH / 'FWUP0024.DAT').read_bytes()

    def extract(self, original):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.startup'),
                             'bootstrap recovery is not implemented')
        from xt4lab.startup import extract_bootstrap
        return extract_bootstrap(original)

    def test_extract_exact_bootstrap(self):
        data = self.extract(self.original)
        self.assertEqual(len(data), 0xc880)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         'bf2d1d4ca9d1d60e5125a964988a3f1852e177c3176d35dc59dcc63afe41f6b1')
        self.assertEqual(struct.unpack_from('<I', data)[0], 0xe59ff018)

    def test_wrong_dat_rejected(self):
        for data in [self.original[:-1], b'X' + self.original[1:]]:
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                self.extract(data)

    def test_bootstrap_profile_stages_from_dat_only(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            root = Path(tmp)
            source = root / 'source'
            source.mkdir()
            shutil.copyfile(RESEARCH / 'FWUP0024.DAT', source / 'FWUP0024.DAT')
            run = prepare_run(source, root / 'bootstrap', 'bootstrap-entry-diagnostic')
            self.assertEqual(run.profile.entry, 0)
            self.assertEqual(run.profile.registers['sp'], 0)
            self.assertEqual(len(run.profile.regions), 1)
            r = run.profile.regions[0]
            self.assertEqual((r.name, r.base, r.size, r.file_size),
                             ('bootstrap', 0, 0x1b0c0, 0xc880))
            self.assertEqual((run.directory / 'inputs/bootstrap.bin').read_bytes(),
                             self.extract(self.original))
            derived = run.manifest['derived_images']['bootstrap']
            self.assertEqual(derived['original_dat_offset'], 0x20274)
            self.assertEqual(derived['transform'], 'xor-ff')
            self.assertEqual(derived['sha256'], r.sha256)
            verify_prepared(run)

    def test_bootstrap_private_copy_checked(self):
        with tempfile.TemporaryDirectory(dir=REPO / 'runs') as tmp:
            run = prepare_run(RESEARCH, Path(tmp) / 'bootstrap', 'bootstrap-entry-diagnostic')
            image = run.directory / 'inputs/bootstrap.bin'
            image.chmod(0o600)
            image.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                verify_prepared(run)


if __name__ == '__main__':
    unittest.main()
