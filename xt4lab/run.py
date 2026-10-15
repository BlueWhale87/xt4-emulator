"""Bounded local execution with evidence retained across resets and failures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import signal
import tempfile
import threading
import time
from .firmware import REPO, PreparedRun, profile_dict, read_regular, verify_prepared
from .rsp import RspClient


def write_json(path: Path, data: dict) -> None:
    with tempfile.NamedTemporaryFile('w', dir=path.parent, delete=False) as f:
        tmp = Path(f.name)
        json.dump(data, f, indent=2)
        f.write('\n')
    os.replace(tmp, path)


def verified_engine(qemu: Path) -> dict:
    if qemu.resolve() != REPO / 'build/qemu-system-arm':
        raise ValueError('Only the recorded local engine may run')
    info = json.loads(read_regular(REPO / 'build', 'xt4-build.json'))
    data = read_regular(REPO / 'build', 'qemu-system-arm')
    if hashlib.sha256(data).hexdigest() != info['binary_sha256']:
        raise ValueError('Engine differs from recorded build; rebuild first')
    return info


class RunSession:
    def __init__(self, prepared: PreparedRun, qemu: Path, *, budget=1000,
                 timeout=2.0, wall_limit=30.0, execution_mode='single-step'):
        self.engine = verified_engine(qemu)
        if execution_mode not in ('single-step', 'bounded-continue'):
            raise ValueError('Invalid execution mode')
        maximum = 1000000000 if execution_mode == 'bounded-continue' else 1000000
        if (type(budget) is not int or not 1 <= budget <= maximum
                or not 0 < timeout <= 2 or not 0 <= wall_limit <= 30):
            raise ValueError('Invalid execution limits')
        self.execution_mode = execution_mode
        self.prepared, self.qemu = prepared, qemu.resolve()
        verify_prepared(prepared)
        self.budget, self.timeout, self.wall_limit = budget, timeout, wall_limit
        self.segment = -1
        self.process = self.client = self._socket_dir = self._stderr = None
        self.stop = None
        self.closed = False
        self.trace = (prepared.directory / 'trace.jsonl').open('x')
        write_json(prepared.directory / 'engine-build.json', self.engine)
        write_json(prepared.directory / 'run-info.json', {
            'schema_version': 1, 'camera_booted': False,
            'profile': profile_dict(prepared.profile), 'provenance': prepared.manifest,
            'budget': budget, 'response_timeout': timeout, 'wall_limit': wall_limit,
            'single_step_flags': 1, 'execution_mode': execution_mode,
            'trace_policy': 'boundary-and-device-events' if self.fast else 'instruction',
            'count_semantics': 'qemu-raw-icount-not-retired' if self.fast else 'acknowledged-single-step'})
        try:
            self._start()
        except BaseException:
            self.close()
            raise

    def _record(self, record):
        self.trace.write(json.dumps({'segment': self.segment, **record}, sort_keys=True) + '\n')
        self.trace.flush()

    @property
    def fast(self):
        return self.execution_mode == 'bounded-continue'

    def _start(self):
        verify_prepared(self.prepared)
        self.segment += 1
        self.segment_dir = self.prepared.directory / 'segments' / f'{self.segment:03d}'
        self.segment_dir.mkdir(parents=True, mode=0o700)
        (self.segment_dir / 'inputs').mkdir(mode=0o700)
        inputs = list(self.prepared.profile.regions)
        if self.prepared.profile.media is not None:
            inputs.append(self.prepared.profile.media)
        for region in inputs:
            if region.file:
                path = self.segment_dir / region.file
                with path.open('xb') as f:
                    f.write(read_regular(self.prepared.directory, region.file))
                path.chmod(0o400)
        (self.segment_dir / 'profile.json').write_text(json.dumps(profile_dict(self.prepared.profile)))
        self._socket_dir = tempfile.TemporaryDirectory(prefix='xt4-', dir='/tmp')
        self.socket_path = Path(self._socket_dir.name).resolve() / 'gdb'
        self.argv = [str(self.qemu), '-machine', 'fujifilm-xt4,run-dir=' + str(self.segment_dir),
            '-accel','tcg,thread=single','-S','-nodefaults','-no-user-config',
            '-display','none','-nic','none','-monitor','none',
            '-semihosting-config','enable=off',
            '-chardev',f'socket,id=xt4gdb,path={self.socket_path},server=on,wait=off',
            '-gdb','chardev:xt4gdb']
        if self.fast:
            self.argv[2] += ',instruction-budget=' + str(self.budget)
        if self.fast or (self.prepared.profile.scenario and self.prepared.profile.scenario.timer):
            self.argv += ['-icount', 'shift=0,align=off,sleep=off']
        self._stderr = (self.segment_dir / 'engine-stderr.log').open('xb')
        self.process = subprocess.Popen(self.argv, cwd=REPO, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=self._stderr, umask=0o077,
            start_new_session=True)
        deadline = time.monotonic() + 2
        while not self.socket_path.exists():
            if self.process.poll() is not None:
                raise RuntimeError('Engine failed during startup; see engine-stderr.log')
            if time.monotonic() >= deadline:
                raise TimeoutError('Engine debugger socket did not appear')
            time.sleep(.01)
        self.client = RspClient(self.socket_path, self.timeout)
        self.socket_path.chmod(0o600)
        if self.client.command('Qqemu.sstep=1') != 'OK':
            raise ValueError('Engine cannot set explicit single-step behavior')
        registers = self.client.registers()
        self.started = time.monotonic()
        self.client.deadline = self.started + self.wall_limit
        self.completed_steps = self.attempted_steps = 0
        self.engine_instruction_count = 0 if self.fast else None
        self._snapshot_sequence = 0
        self.stop = None
        self.paused = False
        self.client.interrupt_requested = lambda: self.paused
        self._record({'event':'start', 'registers':registers, 'argv':self.argv,
                       'engine_events':str(self.segment_dir.relative_to(self.prepared.directory) / 'engine-events.jsonl')})

    def _finish(self, reason, **fields):
        self.stop = {'reason':reason,'completed_steps':self.completed_steps,
                     'attempted_steps':self.attempted_steps, 'segment':self.segment,
                     **self._accounting(), **fields}
        self._record({'event':'stop', **self.stop})
        write_json(self.prepared.directory / 'stop.json', self.stop)
        return self.stop

    def _engine_stop(self):
        file = self.segment_dir / 'engine-events.jsonl'
        if not file.exists():
            return None
        for line in file.read_text().splitlines():
            event = json.loads(line)
            if event.get('event') == 'stop':
                return event
        return None

    def step(self) -> dict:
        if self.fast:
            raise ValueError('step requires single-step mode')
        self.paused = False
        return self._perform_step()

    def _perform_step(self) -> dict:
        return self._with_pause_handler(self._step_impl)

    def _with_pause_handler(self, operation) -> dict:
        # Keep the packet parser on its stack when SIGINT arrives. The handler
        # only requests a pause; RSP polling interrupts execution and drains its
        # stop reply. This also protects partially received ordinary packets.
        main_thread = threading.current_thread() is threading.main_thread()
        previous = None
        if main_thread:
            previous = signal.signal(signal.SIGINT, lambda *_: setattr(self, 'paused', True))
        try:
            return operation()
        finally:
            if main_thread:
                signal.signal(signal.SIGINT, previous)

    def _paused_result(self):
        self._record({'event':'pause','completed_steps':self.completed_steps, **self._accounting()})
        return {'reason':'paused','completed_steps':self.completed_steps,
                'attempted_steps':self.attempted_steps, **self._accounting()}

    def _accounting(self):
        return {'engine_instruction_count': self.engine_instruction_count} if self.fast else {}

    def _execution_snapshot(self):
        path = self.segment_dir / 'engine-events.jsonl'
        records = [json.loads(line) for line in path.read_text().splitlines()]
        states = [e for e in records if e.get('event') == 'execution-state']
        return states[-1] if states else None

    def continue_fast(self) -> dict:
        if not self.fast:
            raise ValueError('continue_fast requires bounded-continue mode')
        self.paused = False
        return self._with_pause_handler(self._fast_impl)

    def _fast_failure(self, reason, **fields):
        self._close_engine()
        return self._finish(reason, **fields)

    def _fast_impl(self) -> dict:
        if self.closed:
            raise ValueError('Session closed')
        if self.stop:
            return self.stop
        # No earlier pause snapshot may stand in for a new failed execution.
        self.engine_instruction_count = None
        self._record({'event': 'continue', 'engine_instruction_count': None})
        if self.process.poll() is not None:
            return self._fast_failure('engine_crash', exit_code=self.process.returncode)
        if time.monotonic() >= self.client.deadline:
            return self._fast_failure('timeout')
        try:
            reply = self.client.continue_run()
            if reply.exited:
                return self._fast_failure('engine_crash', exit_code=reply.exit_code)
            snapshot = self._execution_snapshot()
            if (snapshot is None or type(snapshot.get('sequence')) is not int
                    or snapshot['sequence'] <= self._snapshot_sequence
                    or snapshot.get('running') is not False
                    or type(snapshot.get('engine_instruction_count')) is not int
                    or not 0 <= snapshot['engine_instruction_count'] <= self.budget
                    or snapshot.get('instruction_budget') != self.budget
                    or snapshot.get('count_semantics') != 'qemu-raw-icount-not-retired'):
                raise ValueError('Missing fresh stopped engine accounting')
            self._snapshot_sequence = snapshot['sequence']
            self.engine_instruction_count = snapshot['engine_instruction_count']
            self._record({**snapshot, 'event': 'engine-count'})
            if self.client.execution_timed_out:
                return self._fast_failure('timeout')
            fault = self._engine_stop()
            if fault:
                fault.pop('event', None)
                return self._finish(**fault)
            if reply.signal == 2 and self.paused:
                return self._paused_result()
            if reply.signal != 5:
                return self._fast_failure('unexpected_exception', signal=reply.signal)
            if self.paused:
                return self._paused_result()
            raise ValueError('Continue halted without an engine stop')
        except TimeoutError:
            return self._fast_failure('timeout')
        except (ConnectionError, OSError):
            return self._fast_failure('engine_crash' if self.process.poll() is not None
                                      else 'debugger_disconnected')
        except ValueError as exc:
            return self._fast_failure('debugger_protocol_error', detail=str(exc))

    def _step_impl(self) -> dict:
        if self.closed:
            raise ValueError('Session closed')
        if self.stop:
            return self.stop
        if self.process.poll() is not None:
            return self._finish('engine_crash', exit_code=self.process.returncode)
        if self.completed_steps >= self.budget:
            return self._finish('instruction_budget')
        if time.monotonic() - self.started >= self.wall_limit:
            return self._finish('timeout')
        try:
            registers = self.client.registers()
            if self.paused:
                return self._paused_result()
            pc = registers['pc']
            try:
                raw = self.client.read_memory(pc,4).hex()
            except ValueError:
                raw = None  # Let real CPU execution expose the fetch failure.
            self.attempted_steps += 1
            self._record({'event':'attempt', 'pc':pc, 'bytes':raw, 'cpsr':registers['cpsr']})
            reply = self.client.step()
            fault = self._engine_stop()
            if fault:
                fault.pop('event',None)
                return self._finish(**fault)
            if reply.exited:
                return self._finish('engine_crash',exit_code=reply.exit_code)
            if reply.signal == 2 and self.paused:
                # An interrupted step is not an acknowledged completion.
                # Preserve the uncertainty rather than guessing retirement.
                self._record({'event':'interrupted-step','pc':pc,'bytes':raw,
                              'confirmed_completion':False})
                return self._paused_result()
            if reply.signal != 5:
                return self._finish('unexpected_exception',signal=reply.signal)
            self.completed_steps += 1
            self._record({'event':'instruction','pc':pc,'bytes':raw,'core':0})
            if self.paused:
                return self._paused_result()
            return {'reason':'step','completed_steps':self.completed_steps,
                    'attempted_steps':self.attempted_steps}
        except TimeoutError:
            return self._finish('timeout')
        except (ConnectionError, OSError):
            return self._finish('engine_crash' if self.process.poll() is not None
                                else 'debugger_disconnected')
        except ValueError as exc:
            return self._finish('debugger_protocol_error',detail=str(exc))

    def continue_steps(self, count: int) -> dict:
        if self.fast:
            raise ValueError('continue_steps requires single-step mode')
        if not 0 < count <= 1000000:
            raise ValueError('Invalid step count')
        self.paused = False
        result = {'reason':'paused','completed_steps':self.completed_steps}
        try:
            for _ in range(count):
                if self.paused:
                    break
                result = self._perform_step()
                if result['reason'] != 'step':
                    break
            if not self.stop and self.completed_steps >= self.budget:
                result = self._finish('instruction_budget')
        except KeyboardInterrupt:
            self.pause()
            result = {'reason':'paused','completed_steps':self.completed_steps}
        return result

    def pause(self) -> None:
        self.paused = True
        self._record({'event':'pause','completed_steps':self.completed_steps})

    def _close_engine(self):
        if self.client is not None:
            self.client.close()
            self.client = None
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait(timeout=1)
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None
        if self._socket_dir is not None:
            self._socket_dir.cleanup()
            self._socket_dir = None

    def reset(self) -> None:
        if self.closed:
            raise ValueError('Session closed')
        self._record({'event':'reset-request'})
        self._close_engine()
        self._start()

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        self._close_engine()
        self.trace.close()
