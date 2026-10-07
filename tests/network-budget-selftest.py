#!/usr/bin/env python3
"""Host checks for receiver verification, case accounting, and the experiment driver."""
import concurrent.futures
import fcntl
import os
import importlib.util
from pathlib import Path
import subprocess
import signal
import shutil
import sys
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('budget', ROOT / 'tests/network-budget-experiment.py')
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


class Configuration(unittest.TestCase):
    def test_gate_checks_production_defaults_without_overrides(self):
        # Compile real headers, then simulate a production-default change without
        # updating the gate. Both profiles remain valid experiment candidates.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ('net/lwip_port/include', 'third_party/lwip/src/include'):
                shutil.copytree(ROOT / relative, root / relative)
            (root / 'tests').mkdir()
            script = root / 'tests/network-budget-config.py'
            shutil.copyfile(ROOT / 'tests/network-budget-config.py', script)
            command = [sys.executable, '-B', str(script)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            options = root / 'net/lwip_port/include/lwipopts.h'
            options.write_text(options.read_text().replace(
                '#define BOAROS_LWIP_POOL_SCALE 4', '#define BOAROS_LWIP_POOL_SCALE 1').replace(
                '#define BOAROS_LWIP_MEM_SCALE 2', '#define BOAROS_LWIP_MEM_SCALE 1'))
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, 'production-default drift accepted')
            self.assertIn('static assertion failed', result.stderr)


class Accounting(unittest.TestCase):
    def setUp(self):
        self.case = budget.parse_case('loopback:nonblocking:bulk:1:rx', 65537, 32)
        self.record = dict(id=0, receiver_bytes=65537, start_ns=10, end_ns=110,
                           elapsed_ns=100, complete=1, rtt_p50_ns=10, rtt_max_ns=20)

    def test_requires_receiver_completion(self):
        self.assertEqual(budget.metrics([self.record], self.case)['receiver_bytes'], 65537)
        for change in ({'receiver_bytes': 65536}, {'complete': 0}, {'id': 1}, {'elapsed_ns': 0}):
            with self.assertRaises(RuntimeError):
                budget.metrics([self.record | change], self.case)
        with self.assertRaises(RuntimeError):
            budget.metrics([self.record, self.record], self.case)

    def test_near_pool_keeps_four_pcbs(self):
        self.assertEqual(budget.parse_case('loopback:blocking:bulk:near:tx', 1, 1)['connections'], 14)
        self.assertEqual(budget.parse_case('tap:blocking:bulk:near:tx', 1, 1)['connections'], 28)
        for name in ('loopback:blocking:bulk:15:tx', 'tap:blocking:bulk:29:tx'):
            with self.assertRaises(ValueError):
                budget.parse_case(name, 1, 1)
        case = budget.parse_case('loopback:blocking:mixed:near:tx', 1, 1)
        self.assertEqual((case['connections'], case['bulk_connections'], case['control_id']), (14, 13, 13))

    def test_requires_control_progress_inside_bulk(self):
        case = budget.parse_case('loopback:blocking:mixed:1:rx', 65537, 2)
        control = self.record | dict(id=1, receiver_bytes=128)
        records = [self.record, control]
        samples = [dict(id=1, sequence=i, start_ns=20 + i * 20, end_ns=30 + i * 20) for i in range(2)]
        self.assertEqual(budget.control_metrics(records, samples, case)['control_requests_during_bulk'], 2)
        with self.assertRaises(RuntimeError):
            budget.control_metrics([self.record | dict(end_ns=15), control], samples, case)

    def test_control_tail_keeps_requests_finishing_after_bulk(self):
        case = budget.parse_case('loopback:blocking:mixed:1:rx', 65537, 2)
        control = self.record | dict(id=1, receiver_bytes=128, end_ns=1000, elapsed_ns=990)
        samples = [dict(id=1, sequence=0, start_ns=20, end_ns=30),
                   dict(id=1, sequence=1, start_ns=100, end_ns=900)]
        result = budget.control_metrics([self.record, control], samples, case)
        self.assertEqual(result['control_requests_during_bulk'], 2)
        self.assertEqual(result['control_rr_during_bulk_p99_ns'], 800)
        self.assertEqual(result['control_rr_fully_contained_p99_ns'], 10)

    def test_same_binary_cannot_impersonate_two_canonical_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory); (work / 'runs').mkdir()
            variants = {name: dict(kernel_sha256='same') for name in ('w8-p1-m1', 'w32-p4-m4')}
            budget.save(work / 'identity.json', dict(cases=[self.case], variants=variants,
                        repeat=3, observe=False, smoke=False, program_sha256='p'))
            for variant in variants:
                for repetition in range(1, 4):
                    boot = work / 'runs' / f'{variant}-{repetition}'; boot.mkdir()
                    budget.save(boot / 'result.json', dict(case=self.case, variant=variant, repetition=repetition,
                        status='passed', metrics=budget.metrics([self.record], self.case), kernel_sha256='same',
                        program_sha256='p', fixture_sha256='f', observation_enabled=False))
            with self.assertRaisesRegex(RuntimeError, 'manifest|profile|managed'):
                budget.summarize(work)

    def test_linux_label_rejected_before_tools_or_output(self):
        args = SimpleNamespace(repeat=1, observe=False, smoke=True, variant=['linux=/unused'], linux_kernel=None, cases='')
        with mock.patch.object(budget, 'case_values', side_effect=AssertionError('reserved label accepted')):
            with self.assertRaisesRegex(ValueError, 'linux|reserved'):
                budget.run(args)

    def test_measurement_lock_conflict_precedes_kernel_or_output_work(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'build/cost').mkdir(parents=True)
            with (root / 'build/cost/measurement.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                args = SimpleNamespace(repeat=3, observe=False, smoke=False, baseline=root / 'unused')
                with mock.patch.object(budget, 'ROOT', root), mock.patch.object(budget, 'digest', side_effect=AssertionError('kernel read')):
                    with self.assertRaisesRegex(RuntimeError, 'measurement.*active|measurement.*lock'):
                        budget.run(args)

    def test_aggregate_rejects_mismatched_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'runs').mkdir()
            identity = dict(cases=[self.case], variants={'current': {'kernel_sha256': 'a'}},
                            repeat=3, observe=False, program_sha256='p')
            budget.save(work / 'identity.json', identity)
            for i in range(3):
                boot = work / 'runs' / str(i); boot.mkdir()
                budget.save(boot / 'result.json', dict(case=self.case, variant='current', repetition=i + 1,
                    status='passed', metrics=budget.metrics([self.record], self.case),
                    kernel_sha256='a' if i < 2 else 'changed', program_sha256='p',
                    fixture_sha256='f', observation_enabled=False))
            with self.assertRaises(RuntimeError):
                budget.summarize(work)


class Workload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        cls.program = cls.work / 'tcp-budget'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'tests/workloads/network/tcp-budget.c'), '-o', str(cls.program)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_start_barriers_tolerate_arrival_skew(self):
        program = self.work / 'tcp-budget-stagger'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'tests/workloads/network/tcp-budget.c'),
                        str(ROOT / 'tests/host/network_budget_barrier.c'),
                        '-Wl,--wrap=connect', '-Wl,--wrap=read', '-o', str(program)], check=True)
        config = self.work / 'stagger-config'
        config.write_text('loopback blocking rr 5 262144 32 rx\n')
        process = subprocess.Popen([str(program), str(config)], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            try:
                output, _ = process.communicate(timeout=3)
            except subprocess.TimeoutExpired as error:
                self.fail('start barrier stalled: ' + (error.output or b'').decode(errors='replace'))
            self.assertEqual(process.returncode, 0, output)
            self.assertIn('BUDGET PASS all', output)
        finally:
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait()

    def test_loopback_content_and_modes(self):
        for mode in ('blocking', 'nonblocking'):
            for kind, direction, count in (('bulk', 'rx', 5), ('bulk', 'tx', 14), ('rr', 'rx', 1), ('rr', 'rx', 5), ('rr', 'rx', 14), ('mixed', 'tx', 5), ('mixed', 'rx', 5)):
                with self.subTest(mode=mode, kind=kind, direction=direction):
                    case = budget.parse_case(f'loopback:{mode}:{kind}:{count}:{direction}', 262145, 32)
                    config = self.work / 'config'
                    config.write_text('{path} {mode} {kind} {connections} {bytes} {rounds} {direction}\n'.format(**case))
                    run = subprocess.run([str(self.program), str(config)], text=True, capture_output=True, timeout=20)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('BUDGET PASS all', run.stdout)
                    evidence = Path(str(config) + '.results').read_text()
                    self.assertTrue(evidence.endswith('BUDGET PASS all\n'))
                    records = budget.parse_records(evidence, 'CONNECTION')
                    budget.metrics(records, case)
                    budget.control_metrics(records, budget.parse_records(evidence, 'CONTROL_SAMPLE'), case)

    def test_host_peer_content_and_modes(self):
        for mode in ('blocking', 'nonblocking'):
            for kind, direction, count in (('bulk', 'rx', 5), ('bulk', 'tx', 28), ('rr', 'rx', 1), ('mixed', 'tx', 5), ('mixed', 'rx', 5)):
                with self.subTest(mode=mode, kind=kind, direction=direction):
                    case = budget.parse_case(f'tap:{mode}:{kind}:{count}:{direction}', 262145, 32)
                    config = self.work / 'config'
                    config.write_text('{path} {mode} {kind} {connections} {bytes} {rounds} {direction}\n'.format(**case))
                    process = subprocess.Popen([str(self.program), str(config)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    try:
                        self.assertEqual(process.stdout.readline().strip(), 'BUDGET READY tap')
                        gate = threading.Barrier(case['connections'])
                        with concurrent.futures.ThreadPoolExecutor(case['connections']) as pool:
                            records = list(pool.map(lambda i: budget.host_flow(i, case, gate, '127.0.0.1'), range(case['connections'])))
                        tail, _ = process.communicate(timeout=20)
                        self.assertEqual(process.returncode, 0, tail)
                        self.assertIn('BUDGET PASS all', tail)
                        budget.metrics(records, case)
                        budget.control_metrics(records, [s for r in records for s in r['control_samples']], case)
                    finally:
                        if process.poll() is None:
                            process.kill(); process.wait()


if __name__ == '__main__':
    unittest.main()
