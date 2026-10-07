"""Bootstrap output must select the original target loader and helper ABI."""
import json
import gzip
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
        runtime_options=[]
        if arch=='loongarch':
            runtime=self.root/'runtime.so'
            image=bytearray(80)
            image[:7]=b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HH',image,16,3,258)
            struct.pack_into('<I',image,48,0x43)
            image[64:]=b'runtime-payload!\n'
            runtime.write_bytes(image)
            runtime_options=['--runtime',str(runtime)]
        result = subprocess.run(['python3', '-B', str(HERE / 'prepare.py'),
            '--arch', arch, '--case', str(helper), '--output', str(target),
            *runtime_options,*options], capture_output=True, text=True)
        return result, target

    def test_la_bootstrap_publishes_both_original_interpreter_paths(self):
        result, target = self.generate('loongarch', 258)
        self.assertEqual(result.returncode, 0, result.stderr)
        profile = json.loads(target.read_text())
        script = profile['argv'][3]
        self.assertIn('/lib/ld-linux-loongarch-lp64d.so.1', script)
        self.assertIn(' /lib64/ld-linux-loongarch-lp64d.so.1', script)
        self.assertIn('/usr/lib64/ld-linux-loongarch-lp64d.so.1', script)
        self.assertIn('/lib/ld-musl-loongarch64.so.1', script)
        self.assertIn('ln -s /musl/lib/libc.so /lib64/ld-musl-loongarch-lp64d.so.1', script)
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

    def test_la_kernel_bootstrap_contains_runtime_for_all_musl_groups(self):
        result,target=self.generate('loongarch',258,'--groups','basic')
        self.assertEqual(result.returncode,0,result.stderr)
        profile=json.loads(target.read_text());script=profile['argv'][3]
        self.assertIn('BOAROS_LINUX_SCHED_PRELOAD=/lib/boaros-linux-sched.so',profile['envp'])
        self.assertIn('/tmp/boaros-runtime.sh',script)
        self.assertIn('export LD_PRELOAD=',script)
        marker='BOAROS_RUNTIME_GZIP='
        encoded=script.split(marker,1)[1].split('\n',1)[0].strip("'")
        payload=bytes(int(encoded[i+2:i+5],8) for i in range(0,len(encoded),5))
        self.assertEqual(gzip.decompress(payload),(self.root/'runtime.so').read_bytes())
        self.assertNotIn('if [ "$group" = cyclictest',script)

    def test_rv_bootstrap_does_not_publish_la_runtime(self):
        result,target=self.generate('riscv',243)
        self.assertEqual(result.returncode,0,result.stderr)
        profile=json.loads(target.read_text())
        self.assertIn('BOAROS_LINUX_SCHED_PRELOAD=',profile['envp'])
        self.assertNotIn('BOAROS_RUNTIME_GZIP=',profile['argv'][3])

    def test_runtime_with_wrong_machine_or_soft_float_is_rejected(self):
        runtime=self.root/'wrong.so'
        for machine,flags in [(243,0x43),(258,0x41)]:
            data=bytearray(64);data[:7]=b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HH',data,16,3,machine)
            struct.pack_into('<I',data,48,flags);runtime.write_bytes(data)
            result,target=self.generate('loongarch',258,'--runtime',str(runtime))
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
