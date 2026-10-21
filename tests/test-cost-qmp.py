#!/usr/bin/env python3
"""Paused cost boots pin actual QEMU vCPU threads before executing the guest."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cost_riscv', ROOT / 'tests/cost-riscv.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class QemuPinTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('qemu-system-riscv64'), 'QEMU required')
    def test_actual_single_and_dual_vcpu_threads_are_pinned(self):
        pin = getattr(runner, 'pin_qemu_threads', None)
        self.assertIsNotNone(pin, 'paused QEMU thread pinning is missing')
        available = sorted(os.sched_getaffinity(0))
        if len(available)<3:self.skipTest('requires three allowed host CPUs')
        host_cpus, host_io = available[:2], available[2]
        for count in (1, 2):
            with self.subTest(cpus=count), tempfile.TemporaryDirectory() as temporary:
                socket = Path(temporary) / 'qmp'
                argv = ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'none',
                        '-m', '128M', '-smp', str(count), '-display', 'none',
                        '-accel', 'tcg,thread=multi', '-object', 'iothread,id=cost-io', '-S', '-qmp',
                        f'unix:{socket},server=on,wait=off']
                expected_personality=int(Path('/proc/self/personality').read_text(),16)
                process = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    preexec_fn=lambda: os.sched_setaffinity(0, {host_io}))
                try:
                    actual = pin(process, socket, host_cpus[:count], host_io)
                    self.assertEqual(actual['guest_cpus'], count)
                    self.assertEqual(actual['main']['tid'], process.pid)
                    self.assertEqual(actual['main']['host_cpus'], [host_io])
                    self.assertEqual(len(actual['vcpus']), count)
                    for index, row in enumerate(actual['vcpus']):
                        self.assertEqual(row['cpu_index'], index)
                        self.assertEqual(row['host_cpus'], [host_cpus[index]])
                    self.assertTrue(actual['continued_after_pin'])
                    self.assertEqual(actual['process_personality'],expected_personality)
                    self.assertEqual(len(actual['iothreads']), 1)
                    self.assertEqual(actual['iothreads'][0]['id'], 'cost-io')
                    self.assertEqual(actual['iothreads'][0]['host_cpus'], [host_io])
                    for row in [actual['main'], *actual['vcpus'], *actual['other_threads']]:
                        self.assertEqual(sorted(os.sched_getaffinity(row['tid'])), row['host_cpus'])
                finally:
                    process.terminate()
                    process.communicate(timeout=10)

    @unittest.skipUnless(shutil.which('qemu-system-riscv64'), 'QEMU required')
    def test_guest_already_running_and_wrong_cpu_count_are_rejected(self):
        available = sorted(os.sched_getaffinity(0))
        if len(available)<3:self.skipTest('requires three allowed host CPUs')
        for paused in (True, False):
            with self.subTest(paused=paused), tempfile.TemporaryDirectory() as temporary:
                socket = Path(temporary) / 'qmp'
                argv = ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'none',
                        '-m', '128M', '-smp', '2', '-display', 'none',
                        '-accel', 'tcg,thread=multi', '-qmp',
                        f'unix:{socket},server=on,wait=off']
                if paused:argv.append('-S')
                process = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                try:
                    error = 'vCPU count' if paused else 'ran before'
                    with self.assertRaisesRegex(ValueError, error):
                        runner.pin_qemu_threads(process, socket, available[:1], available[2])
                finally:
                    process.terminate()
                    process.communicate(timeout=10)

    def test_invalid_host_cpu_and_workload_selections_fail_before_build(self):
        available=sorted(os.sched_getaffinity(0))
        common=['python3','-B',str(ROOT/'tests/cost-riscv.py')]
        cases=[(['--metadata-parallel'], 'parallel metadata requires fixed metadata'),
               (['--case','metadata','--metadata-fixed-only','--cpus','2',
                 '--host-vcpu-cpus',str(available[0])], 'one per guest CPU'),
               (['--case','metadata','--metadata-fixed-only','--host-vcpu-cpus',str(available[0]),
                 '--host-io-cpu',str(available[0])], 'must be distinct')]
        for args,message in cases:
            with self.subTest(args=args):
                result=subprocess.run(common+args,text=True,capture_output=True)
                self.assertEqual(result.returncode,2)
                self.assertIn(message,result.stderr)


if __name__ == '__main__':
    unittest.main()
