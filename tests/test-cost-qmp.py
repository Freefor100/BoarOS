#!/usr/bin/env python3
"""Paused cost boots pin actual QEMU vCPU threads before executing the guest."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import qemu_thread_affinity as subject

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


class Qmp:
    def __init__(self, resumed=False):
        self.pending = [{'QMP': {}}]
        self.commands = []
        self.resumed = resumed
        self.statuses = []
        self.cpus = [{'cpu-index': i, 'thread-id': 9000 + i,
                      'qom-path': f'/machine/soc/harts[{i}]', 'target': 'riscv64'}
                     for i in range(4)]

    def __enter__(self): return self
    def __exit__(self, *args): pass
    def settimeout(self, value): pass
    def connect(self, path): pass
    def makefile(self, *args, **kwargs): return self
    def readline(self): return (json.dumps(self.pending.pop(0)) + '\n').encode()

    def write(self, data):
        name = json.loads(data)['execute']
        self.commands.append(name)
        value = {}
        if name == 'query-status':
            value = {'running': self.resumed and self.commands.count(name) > 1}
            self.statuses.append(value)
        elif name == 'stop': self.resumed = False
        elif name == 'query-cpus-fast': value = self.cpus
        elif name == 'query-iothreads': value = []
        self.pending.append({'return': value})


class LogicalAffinityTest(unittest.TestCase):
    def run_pin(self, boot_hart=2, *, logical=True, resumed=False, duplicate=False,
                hart_ids=(0, 1, 2, 3), debugger_failure=False):
        qmp = Qmp(resumed)
        self.qmp = qmp
        masks = {}
        stop = {'boot_hart': boot_hart, 'harts': [
            {'cpu_index': i, 'hart_id': hart} for i, hart in enumerate(hart_ids)],
            'pc': 0x80201234, 'kernel_main_physical': 0x80201234, 'satp': 0,
            'boot_cpu_hardware_id': boot_hart, 'boot_cpu_initialized': 0x43505531,
            'breakpoints_remaining': 0}
        if duplicate: stop['harts'][3]['hart_id'] = 2
        reply = subprocess.CompletedProcess([], int(debugger_failure is True),
            'BOAROS-AFFINITY ' + json.dumps(stop) + '\n', '')
        options = {}
        if logical:
            options = dict(riscv_kernel=Path(__file__), gdb_socket=Path('/tmp/affinity-gdb'))
        with patch.object(subject.socket, 'socket', return_value=qmp), \
             patch.object(Path, 'iterdir', return_value=[SimpleNamespace(name=str(i)) for i in (8000, 9000, 9001, 9002, 9003)]), \
             patch.object(Path, 'read_text', return_value='0'), \
             patch.object(subject.os, 'sched_setaffinity', side_effect=lambda tid, mask: masks.update({tid: set(mask)})), \
             patch.object(subject.os, 'sched_getaffinity', side_effect=lambda tid: masks[tid]), \
             patch('subprocess.run', return_value=reply,
                   side_effect=debugger_failure if isinstance(debugger_failure, BaseException) else None) as debugger:
            result = subject.pin_qemu_threads(SimpleNamespace(pid=8000, poll=lambda: None),
                '/tmp/affinity-qmp', [0, 2, 4, 6], 8, **options)
        return result, qmp, debugger

    def test_boot_hart_two_becomes_logical_cpu_zero_on_host_zero(self):
        result, _, _ = self.run_pin()
        physical = {row['cpu_index']: row['host_cpus'] for row in result['vcpus']}
        self.assertEqual(physical, {0: [2], 1: [4], 2: [0], 3: [6]})

    def test_default_call_preserves_physical_cpu_order(self):
        result, qmp, debugger = self.run_pin(logical=False)
        self.assertEqual([row['host_cpus'] for row in result['vcpus']], [[0], [2], [4], [6]])
        self.assertEqual(qmp.commands[-1], 'cont')
        debugger.assert_not_called()

    def test_hart_identity_is_read_instead_of_assumed_to_equal_cpu_index(self):
        result, _, _ = self.run_pin(hart_ids=(2, 0, 3, 1))
        self.assertEqual([row['host_cpus'] for row in result['vcpus']], [[0], [2], [6], [4]])

    def test_each_boot_hart_keeps_the_remaining_logical_roles_in_hart_order(self):
        for boot, expected in [(0, [[0], [2], [4], [6]]),
                               (1, [[2], [0], [4], [6]]),
                               (3, [[2], [4], [6], [0]])]:
            with self.subTest(boot=boot):
                result, _, _ = self.run_pin(boot)
                self.assertEqual([row['host_cpus'] for row in result['vcpus']], expected)
                self.assertTrue(result['paused_after_gdb_disconnect'])

    def test_resumed_guest_or_duplicate_hart_cannot_fall_back_to_physical_order(self):
        for options, message in [(dict(resumed=True), 'resumed'),
                                 (dict(duplicate=True), 'hart identities')]:
            with self.subTest(options=options), self.assertRaisesRegex(ValueError, message):
                self.run_pin(**options)
            self.assertNotIn('cont', self.qmp.commands)
            self.assertEqual(self.qmp.commands[-2:], ['stop', 'query-status'])
            self.assertFalse(self.qmp.statuses[-1]['running'])

    def test_failed_or_timed_out_debugger_stops_and_confirms_the_guest_is_paused(self):
        for failure in (True, subprocess.TimeoutExpired('gdb', 30)):
            with self.subTest(failure=failure), self.assertRaises((RuntimeError, subprocess.TimeoutExpired)):
                self.run_pin(resumed=True, debugger_failure=failure)
            self.assertNotIn('cont', self.qmp.commands)
            self.assertEqual(self.qmp.commands[-2:], ['stop', 'query-status'])
            self.assertFalse(self.qmp.statuses[-1]['running'])

    def test_kernel_and_gdb_socket_must_be_supplied_together(self):
        for options in [dict(riscv_kernel=__file__), dict(gdb_socket='/tmp/affinity-gdb')]:
            with self.subTest(options=options), self.assertRaisesRegex(ValueError, 'supplied together'):
                subject.pin_qemu_threads(None, None, [0, 2, 4, 6], 8, **options)



if __name__ == '__main__':
    unittest.main()
