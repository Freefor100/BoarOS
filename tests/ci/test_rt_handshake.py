"""A queued READ drain must own later WRITE replies from the same gate."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


nbd = load('nbd_protocol', ROOT / 'tests/host/nbd_fault_test.py')
control = load('nbd_control', ROOT / 'tests/nbd_control.py')


class RtHandshake(unittest.TestCase):
    def test_next_round_waits_for_both_drain_acknowledgements(self):
        observed = {(disk, event) for disk in ('a', 'b')
                    for event in ('READ', 'WRITE', 'FLUSH')}
        # The guest pipe can finish first while either backend pipe still
        # contains held notifications preceding its drain acknowledgement.
        for draining in ({'a', 'b'}, {'a'}, {'b'}):
            self.assertFalse(control.rt_round_complete(True, observed, draining))
        self.assertTrue(control.rt_round_complete(True, observed, set()))
        self.assertFalse(control.rt_round_complete(False, observed, set()))
        observed.remove(('b', 'FLUSH'))
        self.assertFalse(control.rt_round_complete(True, observed, set()))

    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.directory.name) / 'nbd'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-idirafter', str(ROOT / 'include'),
                        str(ROOT / 'tests/host/nbd_fault.c'),
                        str(ROOT / 'tests/host/block_fault.c'),
                        str(ROOT / 'kernel/block.c'), str(ROOT / 'tests/host/block_runtime.c'),
                        '-pthread', '-I', str(ROOT / 'tests/host/block_model'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_read_and_write_held_before_drain_keep_connection_live(self):
        # Every reply is actually held before the controller processes its
        # notification. No sleep determines the drain/release interleaving.
        for commands in ((0, 1), (3, 0, 1), (0, 0, 1), (1, 0, 1)):
            with self.subTest(commands=commands), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                image = work / 'disk.img'
                image.write_bytes(bytes(8192))
                session = nbd.Session(str(self.binary), str(image), str(work / 'disk.sock'),
                                      '--control-stdin')
                session.socket.settimeout(5)
                try:
                    session.process.stdin.write(b'hold\n')
                    session.process.stdin.flush()
                    self.assertEqual(session.process.stderr.readline(), b'control=hold\n')
                    notifications = []
                    for cookie, command in enumerate(commands, 1):
                        data = b'A' * 512 if command == 1 else b''
                        length = 0 if command == 3 else 512
                        session.socket.sendall(struct.pack('>IHHQQI', 0x25609513, 0,
                                                           command, cookie, 0, length) + data)
                        line = session.process.stderr.readline()
                        if command != 0:
                            self.assertIn(b'result=0 ', line)
                            line = session.process.stderr.readline()
                        self.assertTrue(line.startswith(f'held={cookie} '.encode()), line)
                        notifications.append(line)
                    draining = False
                    for line in notifications:
                        draining = control.release_rt_response(session.process.stdin, line, draining)
                    session.process.stdin.flush()
                    for cookie, command in enumerate(commands, 1):
                        self.assertEqual(struct.unpack('>IIQ', nbd.exact(session.socket, 16)),
                                         (0x67446698, 0, cookie))
                        if command == 0:
                            nbd.exact(session.socket, 512)
                    session.cookie = len(commands)
                    try:
                        readback = session.command(0, length=512)
                    except (OSError, AssertionError) as error:
                        self.fail(f'controller disconnected a healthy NBD gate: {error}')
                    self.assertEqual(readback, (0, b'A' * 512))
                    self.assertEqual(session.command(3), (0, b''))
                    self.assertEqual(image.read_bytes()[:512], b'A' * 512)
                    session.close()
                finally:
                    session.socket.close()
                    if session.process.poll() is None:
                        session.process.kill()
                    session.process.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()
