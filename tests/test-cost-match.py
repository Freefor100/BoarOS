#!/usr/bin/env python3
"""Paired campaigns must validate real, complete pilot boots independently."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from cost_measurement import acquire_measurement_lock as acquire_fixture_lock


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n'


def fixture():
    timings = {f'latency-copy-{n}': 10000 for n in (4096, 65536, 1048576)}
    timings.update({f'latency-protect-{n}': 10000 for n in (4096, 1048576)})
    expected = dict(case='latency', seed=123, kernel_sha256='kernel',
                    firmware_sha256='firmware', qemu_sha256='qemu',
                    elf_sha256='elf', fixture_sha256='fixture',
                    runner_source_sha256='runner-source',
                    qemu_path='/frozen/qemu', kernel_source_commit='source',
                    kernel_source_diff_sha256=None)
    invocation = ['/frozen/qemu', '-machine', 'virt', '-bios', '/run/firmware',
                  '-kernel', '/run/kernel', '-m', '512M', '-smp', '1',
                  '-nographic', '-no-reboot', '-drive',
                  'file=/run/latency-0/root.img,if=none,format=raw,id=root,cache=writeback',
                  '-global', 'virtio-mmio.force-legacy=false', '-device',
                  'virtio-blk-device,drive=root,bus=virtio-mmio-bus.0',
                  '-seed', '123', '-dtb', '/run/latency-0/boot.dtb']
    before = dict(kernel_sha256='kernel', firmware_sha256='firmware',
                  qemu_sha256='qemu', elf_sha256='elf', fixture_sha256='fixture',
                  source_sha256='runner-source',
                  fixture_input_sha256='fixture', fixture_mode='frozen',
                  cost_diagnostics=0, replicas=1, replica=0, acceptance=False,
                  transport='modern', cache='writeback', two_disks=False,
                  platform='boaros', platform_config='fixture', case='latency',
                  timebase_hz=10000000, qemu_seed=123, dtb_sha256='dtb',
                  kernel_build_identity={'source_commit': 'source',
                                         'kernel_sha256': 'kernel'}, argv=invocation)
    text = canonical(before)
    record = dict(before, input_keys=list(before),
                  input_sha256=hashlib.sha256(text.encode()).hexdigest(),
                  timings_ns=timings, snapshots=[], closing={})
    log = ''.join(f'COST RESULT {name} {ns}\n' for name, ns in timings.items())
    log += ''.join(f'COST EXPECT latency-copy-{n} file 8 {8*n} {8*n}\n'
                   for n in (4096, 65536, 1048576))
    log += ('COST PASS latency\n'
            'BoarOS: task stacks released=0xa min-free=0x12e8 max-used=0xd08\n'
            'BoarOS: PID 1 exited status=0x0 pages=0x1fbd9 heap-live=0x0; shutting down\n')
    return record, text, log, expected


class MatchProtocolTest(unittest.TestCase):
    def setUp(self):
        import cost_match
        self.module = cost_match

    def validate(self, record=None, text=None, log=None, expected=None, status=0):
        original = fixture()
        return self.module.validate_boot(
            original[0] if record is None else record,
            original[1] if text is None else text,
            original[2] if log is None else log,
            original[3] if expected is None else expected,
            status, Path('/run'))

    def test_pilot_keeps_identity_and_full_owner_gate(self):
        record, text, log, expected = fixture()
        result = self.validate(record, text, log, expected)
        self.assertEqual(result['pages'], 0x1fbd9)
        self.assertEqual(result['stacks_released'], 10)
        self.assertFalse(record['acceptance'])
        self.assertEqual(record['replicas'], 1)

    def test_pilot_cannot_be_relabelled_as_original_acceptance(self):
        for field, value in (('replicas', 3), ('replica', 1), ('acceptance', True)):
            record, text, log, expected = fixture()
            frozen = json.loads(text)
            frozen[field] = value
            text = canonical(frozen)
            record.update(frozen, input_sha256=hashlib.sha256(text.encode()).hexdigest())
            with self.assertRaises(ValueError):
                self.validate(record, text, log, expected)

    def test_input_and_projection_must_match_in_both_directions(self):
        record, text, log, expected = fixture()
        record['qemu_seed'] = 124
        with self.assertRaises(ValueError):
            self.validate(record, text, log, expected)
        record, text, log, expected = fixture()
        record['input_keys'].append('timings_ns')
        with self.assertRaises(ValueError):
            self.validate(record, text, log, expected)

    def test_validly_resealed_wrong_inputs_still_fail(self):
        for field in ('kernel_sha256', 'firmware_sha256', 'elf_sha256',
                      'fixture_sha256', 'qemu_sha256', 'source_sha256'):
            record, text, log, expected = fixture()
            frozen = json.loads(text)
            frozen[field] = 'changed'
            text = canonical(frozen)
            record.update(frozen, input_sha256=hashlib.sha256(text.encode()).hexdigest())
            with self.assertRaises(ValueError):
                self.validate(record, text, log, expected)

    def test_extra_qemu_devices_clock_modes_and_budgets_fail(self):
        for extra in (['-icount', 'shift=0'], ['-plugin', 'trace.so'],
                      ['-device', 'virtio-rng-device'], ['-smp', '2']):
            record, text, log, expected = fixture()
            frozen = json.loads(text)
            frozen['argv'] += extra
            text = canonical(frozen)
            record.update(frozen, input_sha256=hashlib.sha256(text.encode()).hexdigest())
            with self.assertRaises(ValueError):
                self.validate(record, text, log, expected)

    def test_terminal_line_is_unique_complete_and_successful(self):
        _, _, log, _ = fixture()
        for bad in (log.replace('status=0x0 pages=', 'status=0x01 pages='),
                    log.replace('heap-live=0x0;', 'heap-live=0x01;'),
                    log.replace('pages=0x1fbd9', 'pages=0x0'),
                    log + log.splitlines()[-1] + '\n',
                    log + 'BoarOS: root finish failure stage=0x1 error=0x1\n',
                    log.replace('COST PASS latency\n', '')):
            with self.assertRaises(ValueError):
                self.validate(log=bad)

    def test_full_tail_and_owner_order_are_required(self):
        _, _, log, _ = fixture()
        lines = log.splitlines(keepends=True)
        for bad in (''.join(lines[:-1]), log.replace('released=0xa', 'released=0x0'),
                    ''.join(lines[:-3] + [lines[-1], lines[-3], lines[-2]])):
            with self.assertRaises(ValueError):
                self.validate(log=bad)

    def test_windows_expectations_and_timings_are_reparsed(self):
        record, text, log, expected = fixture()
        record['timings_ns']['latency-copy-4096'] = 1
        with self.assertRaises(ValueError):
            self.validate(record, text, log, expected)
        _, _, log, _ = fixture()
        for bad in (log.replace('file 8 32768 32768', 'file 7 32768 32768'),
                    log.replace('COST RESULT latency-copy-4096 10000\n', ''),
                    log + 'COST RESULT latency-copy-4096 10000\n'):
            with self.assertRaises(ValueError):
                self.validate(log=bad)

    def test_nonzero_process_exit_cannot_be_reused_as_success(self):
        for code in (1, -9, 124):
            with self.assertRaises(ValueError):
                self.validate(status=code)

    def test_locking_checks_all_168_native_child_statuses(self):
        record, _, _, expected = fixture()
        expected['case'] = record['case'] = 'locking'
        record['argv'] = [v.replace('latency-0', 'locking-0') for v in record['argv']]
        timings, expectations, statuses = {}, {}, []
        phases = [(r, 0, n) for r in range(3) for n in (1, 8, 32)]
        phases += [(1, op, 8) for op in range(1, 5)]
        for relation, operation, waiters in phases:
            name = f'locking-{relation}-{operation}-{waiters}'
            jobs = waiters + 1
            calls = jobs - (operation in (3, 4))
            timings[name] = 10000
            expectations[name] = f'file {calls} {calls*8192} {calls*8192}'
            for i in range(jobs):
                status = 15 if operation == 3 and i == 0 else 0
                statuses.append(f'COST STATUS {name} {i} {status}\n')
        self.assertEqual(len(statuses), 168)
        frozen = {key: record[key] for key in record['input_keys']}
        text = canonical(frozen)
        record.update(input_sha256=hashlib.sha256(text.encode()).hexdigest(), timings_ns=timings)
        log = ''.join(f'COST RESULT {name} {ns}\nCOST EXPECT {name} {expectations[name]}\n'
                      for name, ns in timings.items()) + ''.join(statuses)
        log += ('COST PASS locking\nBoarOS: task stacks released=0xaa min-free=0x100 max-used=0x200\n'
                'BoarOS: PID 1 exited status=0x0 pages=0x1fbd9 heap-live=0x0; shutting down\n')
        self.validate(record, text, log, expected)
        for bad in (log.replace(statuses[0], ''), log + statuses[0],
                    log.replace('COST STATUS locking-0-0-32 0 0', 'COST STATUS locking-0-0-32 0 15')):
            with self.assertRaises(ValueError):
                self.validate(record, text, bad, expected)

    def test_schedule_is_fixed_complete_and_pairs_same_seed(self):
        plan = self.module.build_schedule(('latency', 'locking'))
        self.assertEqual(len(plan), 144)
        for stage in ('calibration', 'comparison'):
            for case in ('latency', 'locking'):
                rows = [r for r in plan if r['stage'] == stage and r['case'] == case]
                self.assertEqual(len(rows), 36)
                for block in range(12):
                    triplet = [r for r in rows if r['block'] == block]
                    self.assertEqual([r['position'] for r in triplet], ['before', 'middle', 'after'])
                    self.assertEqual(len({r['seed'] for r in triplet}), 1)
                self.assertEqual(len({r['seed'] for r in rows}), 3)
        with self.assertRaises(ValueError):
            self.module.build_schedule(('latency', 'latency'))

    def test_manifest_is_sealed_and_no_missing_or_duplicate_schedule(self):
        document = {'protocol': self.module.PROTOCOL, 'cases': ['latency'],
                    'schedule': self.module.build_schedule(('latency',))}
        sealed = self.module.seal_manifest(document)
        self.module.validate_manifest(sealed)
        for change in ('missing', 'duplicate', 'seed', 'seal'):
            bad = copy.deepcopy(sealed)
            if change == 'missing':
                bad['schedule'].pop()
            elif change == 'duplicate':
                bad['schedule'].append(copy.deepcopy(bad['schedule'][0]))
            elif change == 'seed':
                bad['schedule'][0]['seed'] += 1
            else:
                bad['manifest_sha256'] = 'changed'
            if change != 'seal':
                bad = self.module.seal_manifest(bad)
            with self.assertRaises(ValueError):
                self.module.validate_manifest(bad)


class MatchControllerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(__file__).with_name('cost-match-riscv.py')
        spec = importlib.util.spec_from_file_location('cost_match_driver', path)
        cls.driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.driver)

    def test_calibration_failure_skips_only_affected_case_without_optional_retry(self):
        import cost_match
        manifest = cost_match.seal_manifest(dict(protocol=cost_match.PROTOCOL,
            cases=['latency', 'locking'], schedule=cost_match.build_schedule(('latency', 'locking'))))
        executed = []
        def boot(plan, entry, output, lease, policy):
            executed.append(entry)
            ns = 1000
            if entry['case'] == 'latency' and entry['position'] == 'middle':
                ns = 1100
            return dict(entry, status='completed', writable_root=str(len(executed)),
                        dtb_sha256='dtb', owners={'pages': 123},
                        timings_ns={cost_match.TARGETS[entry['case']]: ns})
        with tempfile.TemporaryDirectory() as folder:
            with patch.object(self.driver, 'run_boot', side_effect=boot), patch('builtins.print'):
                rows, reports = self.driver.campaign(manifest, Path(folder), None, None)
        self.assertEqual(len(rows), 144)
        self.assertEqual(len(executed), 108)
        self.assertFalse(any(row['case'] == 'latency' and row['stage'] == 'comparison'
                             for row in executed))
        self.assertEqual(reports['latency']['calibration']['classification'], 'unresolved')
        self.assertEqual(reports['locking']['comparison']['classification'], 'pass')

    def test_incomplete_duplicate_triplet_or_reused_fixture_cannot_be_analysed(self):
        import cost_match
        plan = dict(schedule=cost_match.build_schedule(('latency',)))
        rows = [dict(entry, status='completed', writable_root=str(i), dtb_sha256='dtb',
                     owners={'pages': 123}, timings_ns={'latency-copy-4096': 1000})
                for i, entry in enumerate(plan['schedule']) if entry['stage'] == 'calibration']
        result = self.driver.stage_report(plan, rows[:-1], 'calibration', 'latency')
        self.assertEqual(result['classification'], 'unresolved')
        for change in ('duplicate', 'reused', 'dtb', 'pages'):
            bad = copy.deepcopy(rows)
            if change == 'duplicate':
                bad[1] = copy.deepcopy(bad[0])
            elif change == 'reused':
                bad[1]['writable_root'] = bad[0]['writable_root']
            elif change == 'dtb':
                bad[1]['dtb_sha256'] = 'changed'
            else:
                bad[1]['owners']['pages'] = 122
            with self.assertRaises(ValueError):
                self.driver.stage_report(plan, bad, 'calibration', 'latency')

    def test_offline_verifier_rejects_fabricated_success_without_boots(self):
        import cost_match
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = cost_match.seal_manifest(dict(protocol=cost_match.PROTOCOL,
                cases=['latency'], schedule=cost_match.build_schedule(('latency',))))
            (root / 'manifest.json').write_text(canonical(manifest))
            (root / 'result.json').write_text(canonical(dict(protocol=cost_match.PROTOCOL,
                status='passed', boots_executed=0, manifest_sha256=manifest['manifest_sha256'],
                restoration={'event': 'not-acquired', 'errors': []})))
            with self.assertRaises(ValueError):
                self.driver.verify_campaign(root)

    def test_policy_denial_never_starts_guest_or_changes_the_profile(self):
        import cost_match
        state = dict(policies={'2': {'scaling_governor': 'powersave'},
                               '3': {'scaling_governor': 'powersave'}}, NoNewPrivs='1')
        manifest = cost_match.seal_manifest(dict(protocol=cost_match.PROTOCOL,
            cases=['latency'], schedule=cost_match.build_schedule(('latency',)),
            inputs={'scripts': {}}, host_before=state))
        with tempfile.TemporaryDirectory() as folder:
            with patch('sys.argv', ['cost-match-riscv.py', '--output', folder]), \
                    patch.object(self.driver, 'prepare', return_value=manifest), \
                    patch.object(self.driver, 'host_state', return_value=state), \
                    patch.object(self.driver.PolicyClient, 'acquire', side_effect=RuntimeError('no_new_privs')), \
                    patch.object(self.driver, 'campaign') as start, patch('builtins.print'):
                self.assertEqual(self.driver.main(), 2)
                start.assert_not_called()
            result = json.loads((Path(folder) / 'result.json').read_text())
            self.assertEqual(result['status'], 'environment_blocked')
            self.assertEqual(result['boots_executed'], 0)
            self.assertEqual(result['host_after'], state)
            checked = self.driver.verify_campaign(Path(folder))
            self.assertFalse(checked['performance_verified'])

    def test_global_measurement_lock_covers_policy_restoration(self):
        import cost_match
        state = {'policies': {}}
        manifest = cost_match.seal_manifest(dict(protocol=cost_match.PROTOCOL,
            cases=['latency'], schedule=cost_match.build_schedule(('latency',)),
            inputs={'scripts': {}}, host_before=state))
        events = []
        class Lease:
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self.close()
            def close(self):
                events.append('unlock')
        def restore():
            events.append('restore')
            return {'event': 'not-acquired', 'errors': []}
        with tempfile.TemporaryDirectory() as folder:
            with patch('sys.argv', ['cost-match-riscv.py', '--output', folder]), \
                    patch.object(self.driver, 'prepare', return_value=manifest), \
                    patch.object(self.driver, 'host_state', return_value=state), \
                    patch.object(self.driver, 'acquire_measurement_lock', return_value=Lease()), \
                    patch.object(self.driver.PolicyClient, 'acquire', side_effect=RuntimeError('denied')), \
                    patch.object(self.driver.PolicyClient, 'close', side_effect=restore), patch('builtins.print'):
                self.driver.main()
        self.assertEqual(events, ['restore', 'unlock'])

    def test_boot_group_is_stopped_even_after_its_leader_exits(self):
        read_fd, write_fd = os.pipe()
        program = (f'import os,signal; pid=os.fork(); '
                   f'\nif not pid:\n os.write({write_fd},b"R"); signal.pause()'
                   f'\nelse:\n os.close({write_fd}); os._exit(1)\n')
        process = subprocess.Popen([sys.executable, '-c', program], start_new_session=True,
                                   pass_fds=(write_fd,))
        os.close(write_fd)
        try:
            self.assertTrue(select.select([read_fd], [], [], 10)[0])
            self.assertEqual(os.read(read_fd, 1), b'R')
            self.assertEqual(process.wait(timeout=10), 1)
            self.driver.stop_boot(process)
            self.assertTrue(select.select([read_fd], [], [], 10)[0], 'descendant still owns the pipe')
            self.assertEqual(os.read(read_fd, 1), b'')
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            os.close(read_fd)

    def test_each_window_uses_its_own_calibration_and_secondary_keeps_old_gate(self):
        import cost_match
        plan = dict(schedule=cost_match.build_schedule(('latency',)))
        rows = []
        for i, entry in enumerate(plan['schedule']):
            timings = {'latency-copy-4096': 1000, 'latency-copy-65536': 1000}
            if entry['stage'] == 'calibration' and entry['position'] == 'middle':
                timings['latency-copy-65536'] = 1100
            rows.append(dict(entry, status='completed', writable_root=str(i), dtb_sha256='dtb',
                             owners={'pages': 123}, timings_ns=timings))
        calibration = self.driver.stage_report(plan, rows, 'calibration', 'latency')
        comparison = self.driver.stage_report(plan, rows, 'comparison', 'latency', calibration)
        self.assertEqual(comparison['windows']['latency-copy-4096']['classification'], 'pass')
        self.assertEqual(comparison['windows']['latency-copy-65536']['classification'], 'unresolved')
        aggregate = self.driver.aggregate_verdict({'latency': {'calibration': calibration,
                                              'comparison': comparison}}, ['latency'])
        self.assertEqual(aggregate['status'], 'partial_passed')
        self.assertEqual(aggregate['other_window_checks']['latency-copy-65536']['criterion'],
                         'original-point-estimate-and-drift')

    def test_policy_guard_keeps_authentication_session_for_prevalidated_sudo(self):
        original_popen = subprocess.Popen
        helper = ('import os,sys,json; '
                  'print(json.dumps({"event":"ready","original":{},"requested":{},'
                  '"snapshot":{"session":os.getsid(0)}}),flush=True); '
                  'sys.stdin.readline(); '
                  'print(json.dumps({"event":"restored","errors":[]}),flush=True)')
        with tempfile.TemporaryDirectory() as folder:
            with acquire_fixture_lock(Path(folder) / 'lock') as lease:
                client = self.driver.PolicyClient(Path(folder))
                def launch(command, **kwargs):
                    return original_popen([sys.executable, '-c', helper], **kwargs)
                try:
                    with patch.object(self.driver.subprocess, 'Popen', side_effect=launch), \
                            patch.object(self.driver.os, 'geteuid', return_value=1000):
                        event = client.acquire(lease)
                    self.assertEqual(event['snapshot']['session'], os.getsid(0))
                finally:
                    self.assertEqual(client.close()['errors'], [])

    def test_policy_client_preserves_the_actual_restoration_error_and_snapshot(self):
        original_popen = subprocess.Popen
        restoration = dict(event='restored', errors=['policy2/scaling_max_freq: restore rejected'],
                           snapshot={'policies': {'policy2': {'fields': {'scaling_max_freq': '2500000'}}}})
        helper = ('import sys,json; '
                  'print(json.dumps({"event":"ready"}),flush=True); '
                  'sys.stdin.readline(); '
                  f'print({canonical(restoration).strip()!r},flush=True); sys.exit(2)')
        with tempfile.TemporaryDirectory() as folder:
            with acquire_fixture_lock(Path(folder) / 'lock') as lease:
                client = self.driver.PolicyClient(Path(folder))
                def launch(command, **kwargs):
                    return original_popen([sys.executable, '-c', helper], **kwargs)
                with patch.object(self.driver.subprocess, 'Popen', side_effect=launch), \
                        patch.object(self.driver.os, 'geteuid', return_value=1000):
                    client.acquire(lease)
                self.assertEqual(client.close(), restoration)


if __name__ == '__main__':
    unittest.main()
