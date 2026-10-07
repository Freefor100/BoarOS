"""Bootstrap output must select the original target loader and helper ABI."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def generate(self, arch, machine, *options):
        helper = self.root / ('case-' + arch)
        elf = bytearray(64)
        elf[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<H', elf, 18, machine)
        helper.write_bytes(elf)
        target = self.root / (arch + '.json')
        result = subprocess.run(['python3', '-B', str(HERE / 'prepare.py'),
            '--arch', arch, '--case', str(helper), '--output', str(target),
            *options], capture_output=True, text=True)
        return result, target

    def test_la_bootstrap_publishes_both_original_interpreter_paths(self):
        result, target = self.generate('loongarch', 258)
        self.assertEqual(result.returncode, 0, result.stderr)
        profile = json.loads(target.read_text())
        script = profile['argv'][3]
        self.assertIn('/lib/ld-linux-loongarch-lp64d.so.1', script)
        self.assertIn('/usr/lib64/ld-linux-loongarch-lp64d.so.1', script)
        self.assertIn('/lib/ld-musl-loongarch64.so.1', script)
        self.assertNotIn('ld-linux-riscv64', script)
        self.assertIn('BOAROS_EVAL_ARCH=loongarch', profile['envp'])
        subprocess.run(['sh', '-n', '-c', script], check=True)

    def test_rv_and_la_configuration_do_not_overwrite_each_other(self):
        result, rv = self.generate('riscv', 243, '--groups', 'basic')
        self.assertEqual(result.returncode, 0, result.stderr)
        before = rv.read_bytes()
        result, la = self.generate('loongarch', 258, '--groups', 'busybox')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(rv.read_bytes(), before)
        self.assertIn('BOAROS_EVAL_GROUPS=basic', json.loads(before)['envp'])
        self.assertIn('BOAROS_EVAL_GROUPS=busybox', json.loads(la.read_text())['envp'])
        self.assertIn('/lib/ld-linux-riscv64-lp64d.so.1', json.loads(before)['argv'][3])

    def test_wrong_architecture_helper_is_rejected_before_output(self):
        result, target = self.generate('loongarch', 243)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('supervisor machine', result.stderr)
        self.assertFalse(target.exists())

    def test_explicit_budget_and_exclusions_remain_visible(self):
        result, target = self.generate('loongarch', 258, '--case-timeout', '17',
            '--diagnostic-exclude', 'finite-test')
        self.assertEqual(result.returncode, 0, result.stderr)
        env = json.loads(target.read_text())['envp']
        self.assertIn('BOAROS_LTP_CASE_TIMEOUT=17', env)
        self.assertIn('BOAROS_DIAGNOSTIC_EXCLUDE=finite-test', env)


if __name__ == '__main__':
    unittest.main()
