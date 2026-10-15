import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


class EngineTests(unittest.TestCase):
    def engine(self):
        spec = importlib.util.find_spec('xt4lab.engine') if Path('xt4lab').exists() else None
        self.assertIsNotNone(spec, 'X-T4 engine implementation missing')
        from xt4lab import engine
        return engine

    def test_source_pin_rejects_wrong_base(self):
        engine = self.engine()
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(['git', 'init', '-q', tmp], check=True)
            with self.assertRaises(ValueError):
                engine.check_source(Path(tmp))

    def test_build_config_is_arm_tcg_only(self):
        flags = self.engine().configure_args()
        for flag in ('--target-list=arm-softmmu', '--without-default-features',
                     '--enable-tcg', '--disable-hvf', '--disable-docs',
                     '--disable-user', '--disable-tools'):
            self.assertIn(flag, flags)

    def test_current_source_matches_pin(self):
        self.assertEqual(self.engine().check_source(Path.cwd()),
                         '4fc49f46dc95d4a27de2509e7fceb2931e91faeb')


if __name__ == '__main__':
    unittest.main()
