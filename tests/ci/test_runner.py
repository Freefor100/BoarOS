"""CI success requires complete commands and every required job."""
import importlib.util
import json
import fnmatch
import yaml
import os
from pathlib import Path
import select
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from arch_profiles import PROFILES

spec = importlib.util.spec_from_file_location('ci_runner', Path(__file__).with_name('run.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Execution(unittest.TestCase):
    def test_failed_case_does_not_mask_following_case_or_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            cases = [
                {'name': 'bad', 'argv': [sys.executable, '-c', 'print("failed"); raise SystemExit(7)'], 'timeout': 2},
                {'name': 'good', 'argv': [sys.executable, '-c', 'print("x"*12000); print("FINAL")'], 'timeout': 2},
            ]
            result = runner.run_cases(cases, Path(directory))
            self.assertEqual(result['status'], 'failed')
            self.assertEqual([row['status'] for row in result['cases']], ['failed', 'passed'])
            self.assertEqual(result['cases'][0]['exit'], 7)
            self.assertTrue((Path(directory) / 'good.log').read_text().endswith('FINAL\n'))
            self.assertTrue((Path(directory) / 'report.json').is_file())

    def test_timeout_and_missing_executable_are_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            result = runner.run_cases([
                {'name': 'slow', 'argv': [sys.executable, '-c', 'import time; time.sleep(5)'], 'timeout': .1},
                {'name': 'missing', 'argv': [str(Path(directory) / 'absent')], 'timeout': 1},
            ], Path(directory))
            self.assertEqual([row['status'] for row in result['cases']], ['timeout', 'runner-error'])
            self.assertEqual(result['status'], 'failed')
            self.assertEqual((Path(directory) / 'slow.log').read_bytes(), b'')

    def test_timeout_preserves_output_after_slow_fixture_is_ready(self):
        read_fd, write_fd = os.pipe()
        original_popen = subprocess.Popen
        with os.fdopen(read_fd, 'rb', buffering=0) as ready, \
                os.fdopen(write_fd, 'wb', buffering=0) as publish, \
                tempfile.TemporaryDirectory() as directory:
            # 准备握手不属于被测wait预算；真实子进程flush后才开始验证超时。
            def launch(argv, **kwargs):
                process = original_popen(argv, pass_fds=(publish.fileno(),), **kwargs)
                publish.close()
                try:
                    self.assertTrue(select.select([ready], [], [], 10)[0], 'fixture ready timeout')
                    self.assertEqual(ready.read(1), b'R')
                except BaseException:
                    runner.stop(process)
                    raise
                return process

            program = (f'import os,time; time.sleep(.3); '
                       f'print("start",flush=True); print("TAIL",flush=True); '
                       f'os.write({publish.fileno()},b"R"); time.sleep(5)')
            with patch.object(runner.subprocess, 'Popen', side_effect=launch):
                result = runner.run_cases([{'name': 'ready',
                    'argv': [sys.executable, '-c', program], 'timeout': .1}], Path(directory))
            row = result['cases'][0]
            self.assertEqual(result['status'], 'failed')
            self.assertEqual(row['status'], 'timeout')
            self.assertEqual(row['resource_recovery'], 'unverified')
            self.assertLess(row['exit'], 0)
            self.assertEqual((Path(directory) / 'ready.log').read_text(), 'start\nTAIL\n')

    def test_empty_or_duplicate_case_sets_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                runner.run_cases([], Path(directory))
            case = {'name': 'same', 'argv': [sys.executable, '-c', 'pass'], 'timeout': 1}
            with self.assertRaises(ValueError):
                runner.run_cases([case, case], Path(directory))

    def test_each_required_job_must_succeed(self):
        required = ['host', 'rv', 'la', 'runtime']
        self.assertTrue(runner.jobs_passed(required, dict.fromkeys(required, {'result': 'success'})))
        for status in ('failure', 'cancelled', 'skipped', ''):
            jobs = dict.fromkeys(required, {'result': 'success'})
            jobs['la'] = {'result': status}
            self.assertFalse(runner.jobs_passed(required, jobs))
        self.assertFalse(runner.jobs_passed(required, {'rv': {'result': 'success'}}))

    def test_extended_plan_executes_strict_inventory_on_both_memories(self):
        for arch in ('riscv', 'loongarch'):
            with self.subTest(arch=arch):
                result = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('run.py')),
                    '--arch', arch, '--suite', 'extended', '--list'], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                cases = json.loads(result.stdout)
                inventory = next(row for row in cases if row['name'] == 'original-programs')
                self.assertIn('--require-pass', inventory['argv'])
                self.assertIn('--reuse-builds', inventory['argv'])
                self.assertEqual([inventory['argv'][i+1] for i, arg in enumerate(inventory['argv'])
                                  if arg == '--memory'], ['512M', '1G'])


class ArchitectureCoverage(unittest.TestCase):
    def plan(self, arch, suite):
        result = subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('run.py')),
            '--arch', arch, '--suite', suite, '--list'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_both_architectures_declare_core_runtime_and_platform(self):
        for arch in ('riscv', 'loongarch'):
            for suite in ('core', 'runtime', 'platform'):
                with self.subTest(arch=arch, suite=suite):
                    cases = self.plan(arch, suite)
                    self.assertTrue(cases)
                    self.assertTrue(all('INIT_CONFIG=config/init.json' in case['argv'] for case in cases))
                    self.assertTrue(all('TEST_MEMORIES=512M 1G' in case['argv'] for case in cases))

    def test_fixed_reference_restoration_contract_is_retained(self):
        commands = [arg for case in self.plan('host', 'host') for arg in case['argv']]
        self.assertIn('test-references', commands)

    def test_native_template_retains_full_abi_observations_and_failed_disk(self):
        path = Path(__file__).resolve().parents[2] / '.github/workflows/native-contracts.yml'
        steps = yaml.safe_load(path.read_text())['jobs']['contracts']['steps']
        uploads = [step for step in steps if step.get('uses', '').startswith('actions/upload-artifact@')]
        ordinary = next(step for step in uploads if step.get('if') == 'always()')['with']['path'].splitlines()
        failed = next(step for step in uploads if step.get('if') == 'failure()')['with']['path'].splitlines()
        for name in ('linux-512M.log', 'metadata.json', 'boaros-1G.normalized', 'comparison.diff'):
            self.assertTrue(any(fnmatch.fnmatchcase('build/diff-abi/run/' + name, pattern) for pattern in ordinary), name)
        self.assertTrue(any(fnmatch.fnmatchcase('build/diff-abi/run/boaros-1G.img', pattern) for pattern in failed))

    def test_shared_runtime_consumers_include_glibc_and_both_sqlite_modes(self):
        for arch in ('riscv', 'loongarch'):
            with self.subTest(arch=arch):
                commands = [argument for case in self.plan(arch, 'runtime') for argument in case['argv']]
                self.assertIn('test-glibc-' + arch, commands)
                self.assertIn('test-sqlite-rollback-' + arch, commands)
                self.assertIn('test-sqlite-wal-' + arch, commands)


class CompilerCompatibility(unittest.TestCase):
    def test_rv_uses_libatomic_switch_only_when_compiler_accepts_it(self):
        for status, expected in ((0, ['-fno-link-libatomic']), (1, [])):
            with self.subTest(status=status), patch('arch_profiles.subprocess.run') as probe:
                probe.return_value.returncode = status
                self.assertEqual(PROFILES['riscv'].musl_flags('/tmp/musl-gcc'), expected)

    def test_la_retains_integer_abi_and_target_page_alignment(self):
        with patch('arch_profiles.subprocess.run') as probe:
            flags = PROFILES['loongarch'].musl_flags('/tmp/musl-gcc')
            self.assertEqual(flags, [*PROFILES['loongarch'].raw_flags,
                                    '-Wl,-z,max-page-size=16384'])
            probe.assert_not_called()


if __name__ == '__main__':
    unittest.main()
