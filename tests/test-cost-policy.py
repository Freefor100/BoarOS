import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import unittest

from cost_policy import PolicyError, PolicyLease, serve_lease


FIELDS = ('scaling_governor', 'scaling_min_freq', 'scaling_max_freq',
          'energy_performance_preference')


class Driver:
    """A file-backed cpufreq driver enforcing bounds and governor side effects."""

    def __init__(self):
        self.failed = None
        self.mismatch = None
        self.writes = []

    def read(self, path):
        return path.read_text().strip()

    def writable(self, path):
        return bool(path.stat().st_mode & 0o222)

    def write(self, path, value):
        self.writes.append((path, value))
        if self.failed == (path.parent.name, path.name, value):
            self.failed = None
            raise OSError('injected write failure')
        if path.name == 'scaling_min_freq':
            if int(value) > int(self.read(path.parent / 'scaling_max_freq')):
                raise OSError('minimum exceeds maximum')
        if path.name == 'scaling_max_freq':
            if int(value) < int(self.read(path.parent / 'scaling_min_freq')):
                raise OSError('maximum below minimum')
        if self.mismatch == (path.parent.name, path.name, value):
            self.mismatch = None
            value = '2400000'
        path.write_text(value + '\n')
        epp = path.parent / 'energy_performance_preference'
        if path.name == 'scaling_governor' and epp.exists():
            epp.write_text(('performance' if value == 'performance' else 'balance_power') + '\n')


class Fixture:
    def __init__(self, directory, shared=False, minimum='400000', maximum='4500000'):
        self.root = Path(directory)
        for cpu in (2, 3):
            policy = self.root / 'cpufreq' / f'policy{2 if shared else cpu}'
            policy.mkdir(parents=True, exist_ok=True)
            values = {'scaling_governor': 'powersave',
                      'scaling_min_freq': minimum,
                      'scaling_max_freq': maximum,
                      'energy_performance_preference': 'balance_performance',
                      'related_cpus': '2 3' if shared else str(cpu),
                      'scaling_cur_freq': '2399000'}
            for name, value in values.items():
                (policy / name).write_text(value + '\n')
            cpu_dir = self.root / f'cpu{cpu}'
            cpu_dir.mkdir()
            (cpu_dir / 'cpufreq').symlink_to(policy)
        extra = self.root / 'cpufreq' / 'policy4'
        extra.mkdir()
        (extra / 'scaling_governor').write_text('untouched\n')
        self.original = self.files()

    def files(self):
        return {str(path.relative_to(self.root)): path.read_text()
                for path in self.root.rglob('*') if path.is_file()}


class PolicyLeaseTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.directory.name)
        self.driver = Driver()
        self.lease = PolicyLease(self.fixture.root, backend=self.driver)

    def tearDown(self):
        self.lease.restore()
        self.directory.cleanup()

    def test_both_policies_ready_check_and_restore_original_values(self):
        ready = self.lease.acquire()
        self.assertEqual(ready['event'], 'ready')
        self.assertEqual(set(ready['original']), {'policy2', 'policy3'})
        for cpu in (2, 3):
            path = self.fixture.root / 'cpufreq' / f'policy{cpu}'
            self.assertEqual((path / 'scaling_governor').read_text().strip(), 'performance')
            self.assertEqual((path / 'scaling_min_freq').read_text().strip(), '2500000')
            self.assertEqual((path / 'scaling_max_freq').read_text().strip(), '2500000')
        self.assertIn('policies', self.lease.check())
        restored = self.lease.restore()
        self.assertEqual(restored['event'], 'restored')
        self.assertEqual(restored['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)
        self.assertTrue(all(path.name in FIELDS and path.parent.name in ('policy2', 'policy3')
                            for path, _ in self.driver.writes))

    def test_partial_second_policy_failure_restores_first_and_second(self):
        self.driver.failed = ('policy3', 'scaling_min_freq', '2500000')
        with self.assertRaisesRegex(PolicyError, 'injected') as error:
            self.lease.acquire()
        self.assertEqual(error.exception.restoration['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_driver_readback_mismatch_is_blocked_and_restored(self):
        self.driver.mismatch = ('policy3', 'scaling_min_freq', '2500000')
        with self.assertRaisesRegex(PolicyError, 'readback'):
            self.lease.acquire()
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_governor_side_effect_does_not_overwrite_original_epp(self):
        self.lease.acquire()
        self.assertEqual(self.driver.read(self.fixture.root / 'cpufreq/policy2/energy_performance_preference'),
                         'performance')
        self.lease.restore()
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_range_above_request_requires_lowering_min_before_max(self):
        self.directory.cleanup()
        self.directory = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.directory.name, minimum='3000000', maximum='4500000')
        self.lease = PolicyLease(self.fixture.root, backend=self.driver)
        self.lease.acquire()
        self.assertEqual(self.lease.restore()['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_range_below_request_requires_raising_max_before_min(self):
        self.directory.cleanup()
        self.directory = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.directory.name, minimum='400000', maximum='2000000')
        self.lease = PolicyLease(self.fixture.root, backend=self.driver)
        self.lease.acquire()
        self.assertEqual(self.lease.restore()['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_shared_policy_is_written_once_and_restored_once(self):
        self.directory.cleanup()
        self.directory = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.directory.name, shared=True)
        self.lease = PolicyLease(self.fixture.root, backend=self.driver)
        ready = self.lease.acquire()
        self.assertEqual(set(ready['original']), {'policy2'})
        self.assertEqual(self.lease.restore()['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_policy_shared_with_other_cpu_blocks_before_any_write(self):
        (self.fixture.root / 'cpufreq/policy2/related_cpus').write_text('2 4\n')
        with self.assertRaisesRegex(PolicyError, 'outside'):
            self.lease.acquire()
        self.assertEqual(self.driver.writes, [])

    def test_missing_or_unwritable_required_field_blocks_before_writes(self):
        path = self.fixture.root / 'cpufreq/policy3/scaling_max_freq'
        path.unlink()
        with self.assertRaises(PolicyError):
            self.lease.acquire()
        self.assertEqual(self.driver.writes, [])
        path.write_text('4500000\n')
        path.chmod(0o444)
        with self.assertRaisesRegex(PolicyError, 'writable'):
            self.lease.acquire()
        self.assertEqual(self.driver.writes, [])

    def test_absent_optional_epp_is_reported_and_not_created(self):
        for cpu in (2, 3):
            (self.fixture.root / f'cpufreq/policy{cpu}/energy_performance_preference').unlink()
        original = self.fixture.files()
        self.lease.acquire()
        self.assertEqual(self.lease.restore()['errors'], [])
        self.assertEqual(self.fixture.files(), original)

    def test_external_strategy_change_fails_check_and_remains_restorable(self):
        self.lease.acquire()
        (self.fixture.root / 'cpufreq/policy3/scaling_governor').write_text('powersave\n')
        with self.assertRaisesRegex(PolicyError, 'changed'):
            self.lease.check()
        self.assertEqual(self.lease.restore()['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)

    def test_changed_policy_membership_cannot_pass_measurement_check(self):
        self.lease.acquire()
        (self.fixture.root / 'cpufreq/policy3/related_cpus').write_text('3 4\n')
        with self.assertRaisesRegex(PolicyError, 'changed'):
            self.lease.check()

    def test_overlapping_distinct_policies_block_before_writes(self):
        (self.fixture.root / 'cpufreq/policy3/related_cpus').write_text('2 3\n')
        with self.assertRaisesRegex(PolicyError, 'overlap'):
            self.lease.acquire()
        self.assertEqual(self.driver.writes, [])

    def test_restore_failure_is_reported_instead_of_claiming_success(self):
        self.lease.acquire()
        self.driver.failed = ('policy3', 'scaling_governor', 'powersave')
        restored = self.lease.restore()
        self.assertTrue(restored['errors'])
        self.assertEqual(self.driver.read(self.fixture.root / 'cpufreq/policy2/scaling_governor'), 'powersave')
        self.assertEqual(self.driver.read(self.fixture.root / 'cpufreq/policy3/scaling_governor'), 'performance')

    def test_failed_stdout_still_restores(self):
        class BrokenOutput:
            def write(self, text):
                raise BrokenPipeError('controller died')
            def flush(self):
                raise BrokenPipeError('controller died')
        affinity = os.sched_getaffinity(0)
        self.assertNotEqual(serve_lease(self.lease, output=BrokenOutput()), 0)
        self.assertEqual(self.fixture.files(), self.fixture.original)
        self.assertEqual(os.sched_getaffinity(0), affinity)


class PolicyProtocolTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.directory.name)
        self.processes = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
            process.communicate(timeout=10)
        self.directory.cleanup()

    def start(self):
        # 调用真实协议函数的注入 API；生产 CLI 不提供假 sysfs 参数。
        source = ('from pathlib import Path; import sys; '
                  'from cost_policy import PolicyLease, serve_lease; '
                  'sys.exit(serve_lease(PolicyLease(Path(sys.argv[1]))))')
        process = subprocess.Popen([sys.executable, '-B', '-c', source, str(self.fixture.root)],
                                   cwd=Path(__file__).parent, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        self.assertEqual(self.read_record(process)['event'], 'ready')
        return process

    def read_record(self, process):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(timeout=10), 'policy protocol handshake timed out')
        return json.loads(process.stdout.readline())

    def finish(self, process):
        process.wait(timeout=10)
        records = [json.loads(line) for line in process.stdout.read().splitlines()]
        self.assertEqual(records[-1]['event'], 'restored')
        self.assertEqual(records[-1]['errors'], [])
        self.assertEqual(self.fixture.files(), self.fixture.original)
        return records

    def test_check_then_explicit_restore(self):
        process = self.start()
        process.stdin.write('{"command":"check"}\n')
        process.stdin.flush()
        self.assertEqual(self.read_record(process)['event'], 'checked')
        process.stdin.write('{"command":"restore"}\n')
        process.stdin.flush()
        self.finish(process)
        self.assertEqual(process.returncode, 0)

    def test_controller_eof_restores(self):
        process = self.start()
        process.stdin.close()
        process.stdin = None
        self.finish(process)

    def test_sigint_and_sigterm_restore(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signum):
                process = self.start()
                process.send_signal(signum)
                self.finish(process)
                self.assertNotEqual(process.returncode, 0)

    def test_malformed_or_unknown_command_restores_and_blocks(self):
        for line in ('not-json\n', '{"command":"check","extra":1}\n', '{"command":"run"}\n'):
            with self.subTest(line=line):
                process = self.start()
                process.stdin.write(line)
                process.stdin.flush()
                records = self.finish(process)
                self.assertEqual(records[0]['event'], 'blocked')
                self.assertNotEqual(process.returncode, 0)

    def test_cli_has_no_arbitrary_path_or_cpu_option(self):
        for option in ('--sysfs', '--cpu', '--command'):
            process = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('cost_policy.py')),
                                      '--lease', option, str(self.fixture.root)],
                                     capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
        self.assertEqual(self.fixture.files(), self.fixture.original)

    @unittest.skipIf(os.geteuid() == 0, 'non-root CLI rejection requires non-root process')
    def test_real_cli_nonroot_is_blocked_without_write(self):
        process = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('cost_policy.py')), '--lease'],
                                 capture_output=True, text=True)
        self.assertNotEqual(process.returncode, 0)
        record = json.loads(process.stdout)
        self.assertEqual(record['event'], 'blocked')
        self.assertIn('root', record['error'])


if __name__ == '__main__':
    unittest.main()
