import importlib.util
import json
import os
from pathlib import Path
import signal
import tempfile
import threading
import unittest
from xt4lab.firmware import prepare_run

REPO = Path(__file__).resolve().parents[2]
RESEARCH = REPO.parents[1] / 'research/2026-10-14-xt4-firmware'
QEMU = REPO / 'build/qemu-system-arm'


class RunTests(unittest.TestCase):
    def session(self, **kwargs):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.run'), 'run controller missing')
        from xt4lab.run import RunSession
        tmp = tempfile.TemporaryDirectory(dir=REPO / 'runs')
        self.addCleanup(tmp.cleanup)
        prepared = prepare_run(RESEARCH, Path(tmp.name) / 'run with spaces', 'module-entry-diagnostic')
        session = RunSession(prepared, QEMU, **kwargs)
        self.addCleanup(session.close)
        return session

    def test_launch_allowlist(self):
        s = self.session()
        for token in ['-S','tcg,thread=single','-nodefaults','none','-no-user-config']:
            self.assertIn(token, s.argv)
        self.assertIn('-nic',s.argv)
        self.assertFalse(any('tcp:' in token or 'usb-host' in token for token in s.argv))
        self.assertEqual(s.socket_path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(os.getpgid(s.process.pid),s.process.pid)
        self.assertEqual(s.client.registers()['pc'], 0x012910c4)
        self.assertEqual(s.client.registers()['cpsr'] & 0x1ff, 0x1d3)

    def test_controls(self):
        s = self.session()
        self.assertEqual(s.step()['completed_steps'],1)
        s.pause()
        self.assertEqual(s.client.registers()['pc'],0x012910c8)
        s.reset()
        self.assertEqual(s.client.registers()['pc'],0x012910c4)
        self.assertEqual(s.step()['completed_steps'],1)
        s.continue_steps(1)
        self.assertEqual(s.client.registers()['pc'],0x012910cc)
        proc = s.process
        s.close()
        self.assertIsNotNone(proc.poll())
        self.assertFalse(s.socket_path.exists())
        records=[json.loads(x) for x in (s.prepared.directory/'trace.jsonl').read_text().splitlines()]
        self.assertEqual(sum(x['event']=='instruction' for x in records),3)

    def test_stop_bounds(self):
        s = self.session(budget=2)
        self.assertEqual(s.continue_steps(3)['reason'],'instruction_budget')
        self.assertEqual(s.completed_steps,2)

    def test_engine_crash(self):
        s = self.session()
        s.process.kill();s.process.wait()
        self.assertEqual(s.step()['reason'],'engine_crash')
        self.assertTrue((s.prepared.directory/'stop.json').exists())

    def test_debugger_disconnect(self):
        s = self.session()
        s.client.close()
        self.assertEqual(s.step()['reason'],'debugger_disconnected')

    def test_wfi_times_out_with_partial_evidence(self):
        s = self.session(timeout=.15)
        # Fixture patch affects anonymous guest RAM only; never an authentic run.
        self.assertEqual(s.client.command('M12910c4,4:03f020e3'),'OK')
        self.assertEqual(s.step()['reason'],'timeout')
        self.assertEqual(s.completed_steps,0)

    def test_deadline(self):
        s = self.session(wall_limit=0)
        self.assertEqual(s.step()['reason'],'timeout')
        self.assertEqual(s.completed_steps,0)

    def test_tampered_engine_is_rejected(self):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.run'), 'run controller missing')
        from xt4lab.run import RunSession
        with self.assertRaises(ValueError):
            RunSession(None, Path('/usr/bin/true'))

    def test_sigint_pauses_pending_execution_and_drains_reply(self):
        s=self.session(timeout=.5)
        self.assertEqual(s.client.command('M12910c4,4:03f020e3'),'OK')
        timer=threading.Timer(.05,lambda:os.kill(os.getpid(),signal.SIGINT))
        timer.start()
        try:
            result=s.continue_steps(1)
        finally:
            timer.join(1)
        self.assertEqual(result['reason'],'paused')
        # A usable pause must leave the protocol synchronized.
        registers=s.client.registers()
        self.assertIn('pc',registers)
        # WFI still awaits a real modeled wake event. Reset is explicit; the
        # debugger pause must not fabricate an interrupt to clear that state.
        s.reset()
        self.assertEqual(s.continue_steps(1)['reason'],'step')
        trace=(s.prepared.directory/'trace.jsonl').read_text()
        self.assertIn('interrupted-step',trace)

    def test_sigint_during_packet_receipt_is_resumable(self):
        s=self.session()
        original=s.client.sock
        class InterruptSocket:
            fired=False
            def __getattr__(self,name):
                return getattr(original,name)
            def recv(self,n):
                data=original.recv(n)
                if not self.fired:
                    self.fired=True
                    os.kill(os.getpid(),signal.SIGINT)
                return data
        s.client.sock=InterruptSocket()
        self.assertEqual(s.continue_steps(1)['reason'],'paused')
        self.assertEqual(s.completed_steps,0)
        self.assertEqual(s.continue_steps(1)['reason'],'step')
        self.assertEqual(s.completed_steps,1)


if __name__ == '__main__':
    unittest.main()
