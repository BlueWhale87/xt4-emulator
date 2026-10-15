import importlib.util
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_arm', root / 'tools/check_arm.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class ToolchainLockTest(unittest.TestCase):
    def test_requires_explicit_root_and_checks_library_contents(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / 'lib').mkdir()
            lib = path / 'lib/libm.a'
            lib.write_bytes(b'pinned library')
            lock = {'libraries': {'lib/libm.a': {'bytes': lib.stat().st_size,
                    'sha256': hashlib.sha256(lib.read_bytes()).hexdigest()}}}
            with self.assertRaises(m.ArmAuditError):
                m.resolve_toolchain(lock, None)
            actual, libraries = m.resolve_toolchain(lock, path)
            self.assertEqual(actual, path.resolve())
            self.assertEqual(libraries, [lib.resolve()])
            lib.write_bytes(b'changed library')
            with self.assertRaises(m.ArmAuditError):
                m.resolve_toolchain(lock, path)
            lock['libraries']['../outside.a'] = lock['libraries'].pop('lib/libm.a')
            with self.assertRaises(m.ArmAuditError):
                m.resolve_toolchain(lock, path)

if __name__ == '__main__':
    unittest.main()
