import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from xt4lab.firmware import prepare_run
from xt4lab.run import RunSession

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'


class ReportTests(unittest.TestCase):
    def test_report_truthfulness(self):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.report'), 'evidence reporting missing')
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            prepared=prepare_run(RESEARCH,Path(tmp)/'partial','module-entry-diagnostic')
            s=RunSession(prepared,REPO/'build/qemu-system-arm',budget=2)
            try:
                s.continue_steps(2)
            finally:
                s.close()
            report=write_report(prepared.directory)
            self.assertTrue(report.is_file())
            data=json.loads((prepared.directory/'summary.json').read_text())
            self.assertFalse(data['camera_booted'])
            self.assertEqual(data['stop']['reason'],'instruction_budget')
            self.assertEqual(data['completed_steps'],2)
            self.assertIn('synthetic',report.read_text().lower())
            self.assertEqual(json.loads((prepared.directory/'dependencies.json').read_text()),[])

    def test_missing_stop_is_explicit(self):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.report'), 'evidence reporting missing')
        from xt4lab.report import write_report
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            prepared=prepare_run(RESEARCH,Path(tmp)/'unfinished','module-entry-diagnostic')
            s=RunSession(prepared,REPO/'build/qemu-system-arm')
            s.step();s.close()
            write_report(prepared.directory)
            data=json.loads((prepared.directory/'summary.json').read_text())
            self.assertEqual(data['stop']['reason'],'incomplete')
            self.assertFalse(data['camera_booted'])

    def test_cli_relative_research_path(self):
        with tempfile.TemporaryDirectory(dir=REPO/'runs') as tmp:
            path=Path(tmp)/'cli'
            result=subprocess.run([sys.executable,'-m','xt4lab','run','--research-root',
                '../../research/2026-10-14-xt4-firmware','--out',str(path),'--steps','2'],
                cwd=REPO,text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertTrue((path/'report.md').is_file())

if __name__=='__main__':
    unittest.main()
