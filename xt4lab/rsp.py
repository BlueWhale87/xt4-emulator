"""Small synchronous GDB remote client over a private Unix socket."""
from dataclasses import dataclass
from pathlib import Path
import socket
import select
import time
import xml.etree.ElementTree as ET


@dataclass(frozen=True)
class StopReply:
    signal: int | None
    thread: int | None
    exited: bool
    exit_code: int | None


class RspClient:
    def __init__(self, socket_path: Path, timeout: float):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(timeout)
        self.sock.connect(str(socket_path))
        self.timeout = timeout
        self.deadline = None
        self.pending_command = None
        self.interrupt_requested = lambda: False
        self._interrupt_sent = False
        self._response_deadline = None
        self._execution = False
        self.execution_timed_out = False
        self._cleanup_deadline = None
        self._registers = None

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _read(self, count: int, *, stop_prefix=False) -> bytes:
        data = b''
        read_deadline = time.monotonic() + self.timeout
        while len(data) < count:
            if self.sock is None:
                raise ConnectionError('Debugger connection closed')
            now = time.monotonic()
            if self._execution and self.deadline is not None and now >= self.deadline:
                if not self.execution_timed_out:
                    self.execution_timed_out = True
                    self._cleanup_deadline = now + self.timeout
            limits = [] if self._execution and stop_prefix else [read_deadline]
            if self._cleanup_deadline is not None:
                limits.append(self._cleanup_deadline)
            elif self.deadline is not None:
                limits.append(self.deadline)
            if self._response_deadline is not None:
                limits.append(self._response_deadline)
            remaining = min(limits) - now
            if remaining <= 0:
                raise TimeoutError('Execution deadline exhausted')
            self.sock.settimeout(min(self.timeout, remaining))
            if (self.pending_command in ('s', 'c') and stop_prefix
                    and (self.interrupt_requested() or self.execution_timed_out)
                    and not self._interrupt_sent):
                # Never inject an interrupt into a partially received stop frame.
                self.sock.sendall(b'\x03')
                self._interrupt_sent = True
            if not select.select([self.sock], [], [], min(remaining, .02))[0]:
                continue
            chunk = self.sock.recv(count - len(data))
            if not chunk:
                raise ConnectionError('Debugger disconnected')
            data += chunk
        return data

    def command(self, command: str, *, _execution=False) -> str:
        if self.pending_command is not None:
            raise ValueError('A debugger request is still outstanding')
        if _execution and self.deadline is None:
            raise ValueError('Continue requires an absolute execution deadline')
        self.pending_command = command
        self._interrupt_sent = False
        self._execution = _execution
        self.execution_timed_out = False
        self._cleanup_deadline = None
        self._response_deadline = time.monotonic() + self.timeout
        try:
            return self._command(command)
        finally:
            self.pending_command = None
            self._response_deadline = None
            self._execution = False
            self._cleanup_deadline = None

    def _command(self, command: str) -> str:
        if self.sock is None:
            raise ConnectionError('Debugger connection closed')
        data = command.encode('ascii')
        frame = b'$' + data + b'#' + ('%02x' % (sum(data) & 255)).encode()
        for _ in range(3):
            self.sock.sendall(frame)
            ack = self._read(1)
            if ack == b'+':
                break
            if ack != b'-':
                raise ValueError('Invalid debugger acknowledgement')
        else:
            raise ValueError('Debugger rejected packet repeatedly')
        for _ in range(32):
            if self._execution:
                self._response_deadline = None
            if self._read(1, stop_prefix=True) != b'$':
                raise ValueError('Invalid debugger packet prefix')
            if self._execution:
                self._response_deadline = time.monotonic() + self.timeout
            encoded = bytearray()
            while True:
                byte = self._read(1)
                if byte == b'#':
                    break
                encoded += byte
                if len(encoded) > 1048576:
                    raise ValueError('Debugger packet too large')
            checksum = self._read(2)
            if int(checksum, 16) != sum(encoded) & 255:
                self.sock.sendall(b'-')
                continue
            self.sock.sendall(b'+')
            decoded = bytearray()
            i = 0
            while i < len(encoded):
                value = encoded[i]
                if value == ord('}'):
                    i += 1
                    if i == len(encoded):
                        raise ValueError('Truncated debugger escape')
                    decoded.append(encoded[i] ^ 0x20)
                elif value == ord('*'):
                    i += 1
                    if not decoded or i == len(encoded) or encoded[i] < 29:
                        raise ValueError('Invalid debugger repetition')
                    decoded.extend(decoded[-1:] * (encoded[i] - 29))
                else:
                    decoded.append(value)
                i += 1
            answer = decoded.decode('utf-8')
            # Asynchronous guest console packets are not stop replies.
            if answer.startswith('O') and answer != 'OK':
                try:
                    bytes.fromhex(answer[1:])
                except ValueError:
                    return answer
                continue
            return answer
        raise ValueError('No valid debugger response')

    def _xml(self, name: str) -> ET.Element:
        text = ''
        while True:
            response = self.command(f'qXfer:features:read:{name}:{len(text.encode()):x},fff')
            if not response or response[0] not in 'ml':
                raise ValueError('Missing debugger register XML')
            text += response[1:]
            if len(text) > 1048576:
                raise ValueError('Register XML too large')
            if response[0] == 'l':
                # QEMU relies on gdb-target.dtd to bind xi. Never fetch an
                # external DTD: supply that one known namespace locally.
                if 'xi:' in text and 'xmlns:xi' not in text:
                    text = text.replace('<target', '<target xmlns:xi="http://www.w3.org/2001/XInclude"', 1)
                return ET.fromstring(text)

    def _discover_registers(self) -> None:
        self.command('qSupported:xmlRegisters=arm')
        registers = {}
        next_number = 0
        visited = set()
        def visit(name):
            nonlocal next_number
            if name in visited or len(visited) > 32:
                raise ValueError('Invalid register XML includes')
            visited.add(name)
            for child in self._xml(name).iter():
                tag = child.tag.rsplit('}', 1)[-1]
                if tag == 'include':
                    visit(child.attrib['href'])
                elif tag == 'reg':
                    number = int(child.attrib.get('regnum', next_number))
                    registers[child.attrib['name']] = (number, int(child.attrib['bitsize']))
                    next_number = number + 1
        visit('target.xml')
        required = [*(f'r{i}' for i in range(13)), 'sp', 'lr', 'pc', 'cpsr']
        if not all(name in registers for name in required):
            raise ValueError('Required ARM registers absent')
        self._registers = {name: registers[name] for name in required}

    def registers(self) -> dict[str, int]:
        if self._registers is None:
            self._discover_registers()
        result = {}
        for name, (number, bits) in self._registers.items():
            data = bytes.fromhex(self.command(f'p{number:x}'))
            if len(data) * 8 != bits:
                raise ValueError('Invalid register width: ' + name)
            result[name] = int.from_bytes(data, 'little')
        return result

    def read_memory(self, address: int, size: int) -> bytes:
        answer = self.command(f'm{address:x},{size:x}')
        if answer.startswith('E'):
            raise ValueError('Unmapped debugger memory read')
        data = bytes.fromhex(answer)
        if len(data) != size:
            raise ValueError('Short debugger memory read')
        return data

    def step(self) -> StopReply:
        return self._stop_reply(self.command('s'))

    def continue_run(self) -> StopReply:
        return self._stop_reply(self.command('c', _execution=True))

    @staticmethod
    def _stop_reply(reply: str) -> StopReply:
        if reply[:1] in ('W', 'X'):
            return StopReply(None, None, True, int(reply[1:3], 16))
        if reply[:1] not in ('S', 'T'):
            raise ValueError('Not a debugger stop reply')
        thread = None
        for field in reply[3:].split(';'):
            if field.startswith('thread:'):
                thread = int(field[7:].split('.')[-1], 16)
        return StopReply(int(reply[1:3], 16), thread, False, None)
