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


if __name__ == '__main__':
    unittest.main()
