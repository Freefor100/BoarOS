import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class NativeInputs(unittest.TestCase):
    def test_make_forwards_selected_build_directory(self):
        for arch, variable in (('riscv', 'BUILD_DIR'), ('loongarch', 'LA_BUILD')):
            for cost in (0, 1):
                with self.subTest(arch=arch, cost=cost), tempfile.TemporaryDirectory() as directory:
                    result = subprocess.run(['make', '-n', '--no-print-directory',
                        f'COST_DIAGNOSTICS={cost}', f'{variable}={directory}', f'test-sync-{arch}'],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    lines = [line for line in result.stdout.splitlines() if 'tests/sync/native.py --arch' in line]
                    self.assertEqual(len(lines), 1)
                    arguments = shlex.split(lines[0])
                    self.assertIn('--kernel-dir', arguments)
                    self.assertEqual(arguments[arguments.index('--kernel-dir')+1], directory)

    def test_missing_selected_image_cannot_use_another_build(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, '-B', str(ROOT/'tests/sync/native.py'),
                '--arch', 'riscv', '--kernel-dir', directory, '--qemu', 'missing-qemu'],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(f'missing native sync kernel: {directory}/sync-0', result.stderr)
            self.assertNotIn('Traceback', result.stderr)

if __name__ == '__main__': unittest.main()
