import importlib.util
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest


def packet(payload):
    return b'$' + payload + b'#' + ('%02x' % (sum(payload) & 255)).encode()


class RspTests(unittest.TestCase):
    def client(self, handler):
        self.assertIsNotNone(importlib.util.find_spec('xt4lab.rsp'), 'RSP implementation missing')
        from xt4lab.rsp import RspClient
        tmp = tempfile.TemporaryDirectory(prefix='xt4-rsp-', dir='/tmp')
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / 's'
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(path)); server.listen(1)
        errors = []
        def work():
            try:
                with server.accept()[0] as c:
                    c.settimeout(2)
                    handler(c)
            except BaseException as exc:
                errors.append(exc)
            finally:
                server.close()
        thread = threading.Thread(target=work, daemon=True)
        thread.start()
        client = RspClient(path, .5)
        self.addCleanup(client.close)
        def finish():
            client.close(); thread.join(2)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]
        self.addCleanup(finish)
        return client

    @staticmethod
    def request(c):
        data = b''
        while not data.endswith(b'#14'):
            data += c.recv(1)
        return data

    def test_rsp_framing(self):
        def serve(c):
            self.assertEqual(self.request(c), b'$hello#14')
            c.sendall(b'-')
            self.assertEqual(self.request(c), b'$hello#14')
            c.sendall(b'+$ok#00')
            self.assertEqual(c.recv(1), b'-')
            # Fragmented, escaped dollar/hash/brace/asterisk response.
            wire = packet(b'q}\x04}\x03}]}\x0a')
            for byte in wire:
                c.sendall(bytes([byte]))
            self.assertEqual(c.recv(1), b'+')
        self.assertEqual(self.client(serve).command('hello'), 'q$#}*')

    def test_missing_xml_registers(self):
        def serve(c):
            for response in ['qXfer:features:read+',
                             'l<target><feature><reg name="pc" bitsize="32"/></feature></target>']:
                buf = b''
                while b'#' not in buf:
                    buf += c.recv(1)
                c.recv(2)
                c.sendall(b'+' + packet(response.encode()))
                self.assertEqual(c.recv(1), b'+')
        with self.assertRaisesRegex(ValueError, 'register'):
            self.client(serve).registers()

    def test_disconnect_is_not_success(self):
        def serve(c):
            c.recv(100)
        with self.assertRaises(ConnectionError):
            self.client(serve).command('hello')

    def test_qemu_dtd_declared_include_namespace(self):
        def serve(c):
            data = b''
            while b'#' not in data:
                data += c.recv(1)
            c.recv(2)
            c.sendall(b'+' + packet(b'l<?xml version="1.0"?><!DOCTYPE target SYSTEM "gdb-target.dtd"><target><xi:include href="arm-core.xml"/></target>'))
            self.assertEqual(c.recv(1), b'+')
        root = self.client(serve)._xml('target.xml')
        self.assertEqual(list(root)[0].tag, '{http://www.w3.org/2001/XInclude}include')

    def test_absolute_deadline(self):
        def serve(c):
            c.recv(100)
            time.sleep(.15)
        client = self.client(serve)
        client.deadline = time.monotonic() + .03
        with self.assertRaises(TimeoutError):
            client.command('hello')

    @staticmethod
    def read_request(c):
        data = b''
        while b'#' not in data:
            chunk = c.recv(1)
            if not chunk:
                raise ConnectionError('test peer closed before request')
            data += chunk
        return data + c.recv(2)

    def test_continue_waits_beyond_packet_timeout(self):
        def serve(c):
            self.assertEqual(self.read_request(c), packet(b'c'))
            c.sendall(b'+')
            time.sleep(.12)
            c.sendall(packet(b'T05thread:1;'))
            self.assertEqual(c.recv(1), b'+')
        client = self.client(serve)
        client.timeout = .04
        client.deadline = time.monotonic() + .5
        self.assertTrue(hasattr(client, 'continue_run'), 'bounded continue missing')
        self.assertEqual(client.continue_run().signal, 5)
        self.assertFalse(client.execution_timed_out)

    def test_continue_interrupts_once_and_drains_before_next_request(self):
        def serve(c):
            self.read_request(c); c.sendall(b'+')
            self.assertEqual(c.recv(1), b'\x03')
            c.sendall(packet(b'S02'))
            self.assertEqual(c.recv(1), b'+')
            self.assertEqual(self.read_request(c), packet(b'hello'))
            c.sendall(b'+' + packet(b'OK'))
            self.assertEqual(c.recv(1), b'+')
        client = self.client(serve)
        client.deadline = time.monotonic() + .5
        start = time.monotonic()
        client.interrupt_requested = lambda: time.monotonic() - start >= .03
        self.assertTrue(hasattr(client, 'continue_run'), 'bounded continue missing')
        self.assertEqual(client.continue_run().signal, 2)
        self.assertEqual(client.command('hello'), 'OK')

    def test_pause_during_continue_frame_does_not_send_extra_interrupt(self):
        pause = threading.Event()
        def serve(c):
            self.read_request(c); c.sendall(b'+$T05')
            time.sleep(.04)
            tail = packet(b'T05thread:1;')[4:]
            c.sendall(tail)
            self.assertEqual(c.recv(1), b'+')
            self.assertEqual(self.read_request(c), packet(b'hello'))
            c.sendall(b'+' + packet(b'OK'))
            self.assertEqual(c.recv(1), b'+')
        client = self.client(serve)
        original = client.sock
        class PauseInFrame:
            def __getattr__(self, name):
                return getattr(original, name)
            def recv(self, n):
                data = original.recv(n)
                if data == b'T': pause.set()
                return data
        client.sock = PauseInFrame()
        client.interrupt_requested = pause.is_set
        client.deadline = time.monotonic() + .5
        self.assertTrue(hasattr(client, 'continue_run'), 'bounded continue missing')
        self.assertEqual(client.continue_run().signal, 5)
        self.assertEqual(client.command('hello'), 'OK')

    def test_continue_deadline_drains_with_bounded_grace(self):
        def serve(c):
            self.read_request(c); c.sendall(b'+')
            self.assertEqual(c.recv(1), b'\x03')
            time.sleep(.02)
            c.sendall(packet(b'S02'))
            self.assertEqual(c.recv(1), b'+')
        client = self.client(serve)
        client.timeout = .1
        client.deadline = time.monotonic() + .03
        self.assertTrue(hasattr(client, 'continue_run'), 'bounded continue missing')
        self.assertEqual(client.continue_run().signal, 2)
        self.assertTrue(client.execution_timed_out)

    def test_continue_partial_frame_expiry_and_disconnect_are_failures(self):
        for response in [b'+$S05', b'+?', b'+$Sgg#21', b'']:
            with self.subTest(response=response):
                def serve(c):
                    self.read_request(c)
                    if response:
                        c.sendall(response)
                        time.sleep(.12)
                client = self.client(serve)
                client.timeout = .04
                client.deadline = time.monotonic() + .2
                self.assertTrue(hasattr(client, 'continue_run'), 'bounded continue missing')
                with self.assertRaises((TimeoutError, ValueError, ConnectionError)):
                    client.continue_run()

    def test_continue_unresponsive_interrupt_has_finite_cleanup(self):
        def serve(c):
            self.read_request(c); c.sendall(b'+')
            self.assertEqual(c.recv(1), b'\x03')
            time.sleep(.15)
        client = self.client(serve)
        client.timeout = .04
        client.deadline = time.monotonic() + .02
        self.assertTrue(hasattr(client, 'continue_run'), 'bounded continue missing')
        start = time.monotonic()
        with self.assertRaises(TimeoutError):
            client.continue_run()
        self.assertLess(time.monotonic() - start, .13)

    def test_ordinary_command_keeps_packet_bound(self):
        def serve(c):
            self.read_request(c); c.sendall(b'+')
            time.sleep(.12)
        client = self.client(serve)
        client.timeout = .04
        client.deadline = time.monotonic() + .5
        with self.assertRaises(TimeoutError):
            client.command('hello')


if __name__ == '__main__':
    unittest.main()
