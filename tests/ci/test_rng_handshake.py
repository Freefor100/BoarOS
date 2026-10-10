"""The stop probe must wait for a real backend request, without returning entropy."""
import argparse
import importlib.util
import json
from pathlib import Path
import selectors
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
spec = importlib.util.spec_from_file_location('rng_runner', ROOT / 'tests/rng_runner.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class PendingStop(unittest.TestCase):
    def test_stop_waits_for_backend_request_in_either_event_order(self):
        for first in ('guest', 'backend'):
            with self.subTest(first=first), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                kernel = work / 'kernel'
                kernel.write_bytes(b'host fixture kernel')
                guest = work / 'guest.py'
                guest.write_text('''import socket, sys
connection = socket.socket(socket.AF_UNIX)
argument = sys.argv[sys.argv.index('-chardev') + 1]
connection.connect(argument.split('path=', 1)[1])
def marker():
    print('rng: unready\\nrng: stop waiting', flush=True)
if sys.argv[1] == 'guest': marker()
connection.sendall(bytes((2, 64)))
if sys.argv[1] == 'backend': marker()
if sys.stdin.readline() != '\\n': sys.exit(3)
connection.setblocking(False)
try:
    entropy = connection.recv(64)
except BlockingIOError:
    entropy = b''
if entropy: sys.exit(4)
print('rng: stop pending\\nfixture root owners released', flush=True)
''')

                class Profile:
                    name = 'riscv'
                    def boot(self, qemu, image, memory):
                        return [sys.executable, '-u', str(guest), first]
                    def block(self, transport): return 'fixture-block'
                    def rng(self): return 'fixture-rng'
                    def root_success(self, output, status):
                        return status == 42 and 'fixture root owners released' in output

                args = argparse.Namespace(output=work, kernel=kernel, linux_kernel=None,
                    program=kernel, qemu=sys.executable, force_device=False, timeout=5,
                    marker=[])
                delivered = []
                primary = 'guest' if first == 'guest' else 'egd'

                class OrderedSelector(selectors.DefaultSelector):
                    def select(self, timeout=None):
                        ready = super().select(timeout)
                        # 控制真实fd的首次交付，宿主调度不能颠倒待验交错。
                        if not delivered:
                            ready = [(key, mask) for key, mask in ready if key.data == primary]
                        delivered.extend(key.data for key, _ in ready)
                        return ready

                with patch.object(runner, 'quiet'), \
                        patch.object(runner.selectors, 'DefaultSelector', OrderedSelector):
                    runner.run(args, Profile(), 'BoarOS', '1G', 'modern', 's')
                self.assertEqual(delivered[0], primary)
                result = json.loads((work / 'BoarOS-1G-modern-s/result.json').read_text())
                self.assertTrue(result['pass'])
                self.assertTrue(result['stop_request_confirmed'])
                self.assertEqual(result['egd_requested'], 64)
                self.assertFalse(result['released_entropy'])


if __name__ == '__main__':
    unittest.main()
