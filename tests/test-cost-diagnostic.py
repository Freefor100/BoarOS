"""Host counterexamples for diagnostic identity, budget and trace accounting."""
from pathlib import Path
import tempfile
import unittest
import json
import os
import subprocess
import sys
from cost_match import canonical, digest
from cost_measurement import acquire_measurement_lock

import cost_diagnostic as diag


def trace(case='latency'):
    count = len(diag.WINDOW_NAMES[case])
    rows = [f'TRACE 1 {count * 2} 0 1 1 1']
    rows += [f'FLUSH {i} 0' for i in range(count * 2 + 1)]
    # One TB executes twice, has three static instructions; this is a proxy.
    for i in range(count):
        rows.append(f'WINDOW {i} 100 80 0 1 2 {int(i == 0)} {3 if i == 0 else 0} 6 1 2 3 4 5 6 7')
    rows += ['TB 0 1000 2000 3 1 ' + ' '.join(['2 0'] * count), 'END 1']
    return '\n'.join(rows) + '\n'


class DiagnosticTests(unittest.TestCase):
    def test_fixed_schedule_consumes_twelve_slots(self):
        schedule = diag.schedule()
        expected = [('latency', 0), ('locking', 0), ('latency', 1), ('locking', 1),
                    ('latency', 2), ('locking', 2), ('locking', 2), ('latency', 2),
                    ('locking', 1), ('latency', 1), ('locking', 0), ('latency', 0)]
        self.assertEqual([(r['case'], r['seed'] - diag.SEED) for r in schedule], expected)
        with tempfile.TemporaryDirectory() as temporary:
            budget = Path(temporary)
            for entry in schedule:
                diag.reserve_attempt(budget, entry)
                with self.assertRaises(ValueError):
                    diag.reserve_attempt(budget, entry)  # A failed slot cannot be refilled.
            with self.assertRaises(ValueError):
                diag.reserve_attempt(budget, dict(ordinal=12, case='latency', seed=diag.SEED))

    def test_trace_separates_host_wait_from_cpu_and_retired_instructions(self):
        result = diag.parse_trace(trace(), 'latency')
        window = result['windows'][0]
        self.assertEqual(window['wall_minus_thread_cpu_ns'], 20)
        self.assertEqual(window['voluntary_switches'], 1)
        self.assertEqual(window['involuntary_switches'], 2)
        self.assertEqual(window['tb_instruction_proxy'], 6)
        self.assertNotIn('retired_instructions', window)
        self.assertEqual(window['entries']['timer'], 1)
        self.assertEqual(window['entries']['reap'], 5)
        self.assertEqual(window['tb_records'], 1)

    def test_incomplete_duplicate_and_overflow_trace_are_rejected(self):
        valid = trace()
        bad = [valid.replace('END 1\n', ''),
               valid.replace('TRACE 1 10 0 1 1 1', 'TRACE 1 10 1 1 1 1'),
               valid.replace('WINDOW 0 100', 'WINDOW 1 100'),
               valid.replace('FLUSH 0 0', 'FLUSH 1 0'),
               valid.replace('100 80', '80 100'),
               valid.replace(' 1 3 6 ', ' 1 3 7 '),
               valid.replace('TB 0 1000', 'TB 1 1000'),
               valid.replace('TRACE 1 10 0 1 1 1', 'TRACE 1 10 0 2 1 1'),
               valid + 'END 1\n']
        for text in bad:
            with self.subTest(text=text[:70]), self.assertRaises(ValueError):
                diag.parse_trace(text, 'latency')

    def test_generated_but_unexecuted_tb_is_kept(self):
        text = trace().replace('TRACE 1 10 0 1 1 1', 'TRACE 1 10 0 1 1 2')
        text = text.replace('END 1', 'TB 1 1000 2000 1 0 ' + ' '.join(['0 4'] * 5) + '\nEND 2')
        self.assertEqual(diag.parse_trace(text, 'latency')['tb_count'], 2)

    def test_non_simultaneous_clock_skew_has_an_explicit_interval(self):
        text = trace().replace('100 80 0', '100 101 2')
        window = diag.parse_trace(text, 'latency')['windows'][0]
        self.assertEqual(window['wall_minus_thread_cpu_ns'], -1)
        self.assertEqual(window['wall_minus_thread_cpu_interval_ns'], [-3, 1])

    def test_receipt_binds_actual_argv_and_is_diagnostic_only(self):
        expected = dict(ordinal=0, argv=['/qemu', '-smp', '1', '-plugin', 'file=/plugin'],
                        qemu_sha256='q', plugin_sha256='p', manifest_sha256='m',
                        clock_pc=0x1384c, events=[1, 2, 3, 4, 5, 6, 7])
        receipt = dict(expected, profile=diag.PROFILE, acceptance=False)
        diag.validate_receipt(receipt, expected)
        for field, value in [('argv', ['/qemu', '-smp', '2']), ('acceptance', True),
                             ('ordinal', 1), ('clock_pc', 0x13e8c),
                             ('plugin_sha256', 'wrong'), ('profile', 'formal')]:
            changed = dict(receipt, **{field: value})
            with self.subTest(field=field), self.assertRaises(ValueError):
                diag.validate_receipt(changed, expected)

    def test_wrapper_exec_seal_dump_only_and_no_second_boot(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            (out / 'attempts').mkdir()
            qemu, observer = out/'fake-qemu', out/'observer.so'
            # This is a host exec/argv probe, not a QEMU guest or a performance sample.
            qemu.write_text('#!/usr/bin/python3\nimport json,sys\nprint(json.dumps(sys.argv))\n')
            qemu.chmod(0o755)
            observer.write_bytes(b'host-only-observer')
            manifest = diag.sealed(dict(profile=diag.PROFILE, acceptance=False, schedule=diag.schedule(),
                inputs=dict(qemu=dict(path=str(qemu), sha256=digest(qemu)),
                    observer=dict(path=str(observer), sha256=digest(observer)),
                    events={name: dict(pc=i+1) for i, name in enumerate(diag.EVENT_NAMES)},
                    cases=dict(latency=dict(clock_pc=0x1384c)))))
            (out/'manifest.json').write_text(canonical(manifest))
            entry = diag.schedule()[0]
            diag.reserve_attempt(out/'attempts', entry)
            config = diag.execution_config(manifest, entry, out)
            config_path = out/'config.json'
            config_path.write_text(canonical(config))
            wrapper = Path(__file__).with_name('cost-diagnostic-qemu.py')
            def run(arguments):
                return subprocess.run([sys.executable, '-B', str(wrapper), *arguments],
                    env=dict(os.environ, BOAROS_DIAG_CONFIG=str(config_path)), capture_output=True, text=True)
            self.assertEqual(run(['--version']).returncode, 0)
            base = config['expected']['argv'][:-2]
            probe = base[1:-2]
            probe[probe.index('-machine')+1] = 'virt,dumpdtb=' + base[-1]
            self.assertEqual(run(probe).returncode, 0)
            receipt = Path(config['receipt_path'])
            self.assertFalse(receipt.exists())
            changed = base[1:].copy()
            changed[changed.index('-smp')+1] = '2'
            self.assertEqual(run(changed).returncode, 2)
            self.assertFalse(receipt.exists())
            boot = run(base[1:])
            self.assertEqual(boot.returncode, 0, boot.stderr)
            self.assertEqual(json.loads(boot.stdout), config['expected']['argv'])
            diag.validate_receipt(json.loads(receipt.read_text()), config['expected'])
            self.assertEqual(run(base[1:]).returncode, 2)

    def test_manifest_cannot_change_schedule_or_become_acceptance(self):
        manifest = diag.sealed(dict(profile=diag.PROFILE, acceptance=False, schedule=diag.schedule()))
        diag.check_manifest(manifest)
        for field, value in [('schedule', diag.schedule()[:-1]), ('acceptance', True), ('profile', 'formal')]:
            changed = dict(manifest, **{field: value})
            changed.pop('manifest_sha256')
            with self.subTest(field=field), self.assertRaises(ValueError):
                diag.check_manifest(diag.sealed(changed))

    def test_locking_keeps_all_thirteen_original_windows(self):
        result = diag.parse_trace(trace('locking'), 'locking')
        self.assertEqual(len(result['windows']), 13)
        self.assertEqual(result['windows'][2]['name'], 'locking-0-0-32')

    def test_policy_drift_is_not_hidden_by_successful_restoration(self):
        snapshot = dict(policies=dict(cpu2=dict(cpus=[2], fields=dict(min='2500000', max='2500000'))))
        ready = dict(event='ready', snapshot=snapshot, requested=dict(cpu2=dict(min='2500000', max='2500000')))
        diag.validate_policy_snapshot(ready, snapshot)
        changed = dict(policies=dict(cpu2=dict(cpus=[2], fields=dict(min='400000', max='2500000'))))
        with self.assertRaises(ValueError):
            diag.validate_policy_snapshot(ready, changed)
        with self.assertRaises(ValueError):
            diag.validate_policy_snapshot(dict(ready, event='restored'), snapshot)

    def test_preparation_cannot_build_during_another_measurement(self):
        root = Path(__file__).resolve().parents[1]
        (root/'build/cost').mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as temporary, acquire_measurement_lock(root/'build/cost/measurement.lock'):
            result = subprocess.run([sys.executable, '-B', str(root/'tests/cost-diagnostic-riscv.py'),
                                     '--prepare', str(Path(temporary)/'output')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('measurement already running', result.stderr)


if __name__ == '__main__':
    unittest.main()
