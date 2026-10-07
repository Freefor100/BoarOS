"""Report failures and combined scores from the real fixed upstream judge."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('compatibility_runner', HERE / 'run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ReportTests(unittest.TestCase):
    def test_runtime_identity_uses_actual_target_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            (base / 'init.json').write_text(json.dumps({'envp': [
                'BOAROS_LINUX_SCHED_PRELOAD=/lib/boaros-linux-sched.so']}))
            runtime = base / 'linux-sched.so'
            runtime.write_bytes(b'container compiler output')
            info = runner.runtime_info(base)['linux_sched']
            self.assertTrue(info['enabled'])
            self.assertEqual(info['elf_sha256'], runner.sha(runtime))
            runtime.write_bytes(b'other compiler output')
            self.assertNotEqual(runner.runtime_info(base)['linux_sched']['elf_sha256'],
                                info['elf_sha256'])

    def test_disabled_and_historical_runtime_need_no_la_artifact(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            for environment in [[], ['BOAROS_LINUX_SCHED_PRELOAD=']]:
                (base / 'init.json').write_text(json.dumps({'envp': environment}))
                self.assertEqual(runner.runtime_info(base), {
                    'linux_sched': {'enabled': False, 'guest_path': None}})

    def states(self, text, groups, reason='qemu-exit'):
        function = getattr(runner, 'summarize_groups', None)
        self.assertTrue(callable(function), 'runner lacks complete per-group state reporting')
        return function(text, groups, reason)

    def test_unreached_and_unselected_are_not_completed(self):
        groups = self.states('', ['basic'])
        self.assertEqual(len(groups), 22)
        self.assertEqual(groups['basic-musl']['state'], 'not-reached')
        self.assertEqual(groups['ltp-musl']['state'], 'not-selected')

    def test_entered_wrapper_failure_is_not_misclassified_as_unreached(self):
        text = 'BOAROS-EVAL ENTER basic-glibc\nBOAROS-EVAL EXIT basic-glibc status=127\n'
        result = self.states(text, ['basic'])['basic-glibc']
        self.assertEqual(result['state'], 'script-failure')
        self.assertEqual(result['script_exit'], 127)
        self.assertFalse(result['started'])

    def test_budget_timeout_does_not_complete_a_partial_group(self):
        text = 'BOAROS-EVAL ENTER ltp-musl\n#### OS COMP TEST GROUP START ltp-musl ####\n'
        result = self.states(text, ['ltp'], 'total-budget-timeout')['ltp-musl']
        self.assertEqual(result['state'], 'timeout')
        self.assertIsNone(result['script_exit'])

    def test_kernel_fatal_preserves_active_case_and_unreached_groups(self):
        text = ('BOAROS-EVAL ENTER ltp-glibc\nRUN LTP CASE fs_fill\n'
                'BoarOS: fatal trap scause=0xd sepc=0xffffffff80058472 stval=0x38\n')
        groups = self.states(text, ['ltp'], 'kernel-runtime-error')
        self.assertEqual(groups['ltp-glibc']['state'], 'kernel-runtime-error')
        case = groups['ltp-glibc']['cases'][0]
        self.assertEqual(case['state'], 'kernel-runtime-error')
        self.assertEqual(case['owner'], 'kernel')
        self.assertIsNone(case['shell_exit_status'])
        self.assertIsNone(case['wait_status'])
        self.assertEqual(groups['ltp-musl']['state'], 'not-reached')

    def test_skip_and_timeout_remain_visible_in_a_completed_wrapper(self):
        text = ('BOAROS-EVAL ENTER ltp-musl\n#### OS COMP TEST GROUP START ltp-musl ####\n'
                'BOAROS-CASE SKIP command=x reason=controller\n'
                'BOAROS-CASE TIMEOUT-END wait_status=9\n'
                '#### OS COMP TEST GROUP END ltp-musl ####\nBOAROS-EVAL EXIT ltp-musl status=0\n')
        result = self.states(text, ['ltp'])['ltp-musl']
        self.assertEqual(result['state'], 'completed')
        self.assertEqual(result['supervision']['skipped'], 1)
        self.assertEqual(result['supervision']['timed_out'], 1)

    def test_completed_group_preserves_case_load_and_wait_errors(self):
        text = ('BOAROS-EVAL ENTER ltp-glibc\n'
                'RUN LTP CASE missing-program\nBOAROS-CASE EXEC-ERROR errno=2\n'
                'FAIL LTP CASE missing-program : 127\n'
                'RUN LTP CASE broken-wait\nBOAROS-CASE WAIT-ERROR errno=10\n'
                'FAIL LTP CASE broken-wait : 125\n'
                '#### OS COMP TEST GROUP END ltp-glibc ####\nBOAROS-EVAL EXIT ltp-glibc status=0\n')
        result = self.states(text, ['ltp'])['ltp-glibc']
        self.assertEqual(result['state'], 'completed')
        self.assertEqual([(c['name'], c['state'], c['shell_exit_status']) for c in result['cases']],
                         [('missing-program', 'load-error', 127), ('broken-wait', 'runner-error', 125)])
        self.assertEqual(result['cases'][0]['errno'], 2)
        self.assertEqual(result['supervision']['exec_failed'], 1)
        self.assertEqual(result['supervision']['wait_failed'], 1)

    def test_zero_exit_is_not_a_pass_and_inflight_case_stays_incomplete(self):
        text = ('BOAROS-EVAL ENTER ltp-musl\nRUN LTP CASE reported-fail\n'
                'case.c:9: TFAIL: contract differs\ncase.c:10: TCONF: another subcase skipped\n'
                'FAIL LTP CASE reported-fail : 0\n'
                'RUN LTP CASE unobserved-result\n')
        result = self.states(text, ['ltp'], 'total-budget-timeout')['ltp-musl']
        self.assertEqual([(c['name'], c['state']) for c in result['cases']],
                         [('reported-fail', 'reported-failure'), ('unobserved-result', 'incomplete')])
        self.assertIsNone(result['cases'][1]['shell_exit_status'])

    def test_supervision_and_reported_skip_keep_distinct_status_and_wait_evidence(self):
        text = ('BOAROS-EVAL ENTER ltp-musl\nRUN LTP CASE spinning\n'
                'BOAROS-CASE TIMEOUT seconds=300 command=spinning\n'
                'BOAROS-CASE TIMEOUT-END wait_status=9 child_pid=12\nFAIL LTP CASE spinning : 124\n'
                'RUN LTP CASE configured-out\ncase.c:3: TCONF: missing capability\n'
                'FAIL LTP CASE configured-out : 32\n')
        result = self.states(text, ['ltp'])['ltp-musl']['cases']
        self.assertEqual(result[0]['state'], 'supervision-timeout')
        self.assertEqual(result[0]['wait_status'], 9)
        self.assertEqual(result[1]['state'], 'reported-skip')
        self.assertEqual(result[1]['shell_exit_status'], 32)
        self.assertIsNone(result[1]['wait_status'])

    def test_actual_colored_ltp_failure_is_classified_without_changing_output(self):
        outputs = ['tst_device.c:354: \x1b[1;31mTBROK: \x1b[0mFailed to acquire device',
                   'asapi_01    2  \x1b[1;31mTFAIL\x1b[0m  :  missing protocols entry']
        for output in outputs:
            with self.subTest(output=output):
                text = ('BOAROS-EVAL ENTER ltp-glibc\nRUN LTP CASE original-case\n' + output +
                        '\nFAIL LTP CASE original-case : 2\n')
                result = self.states(text, ['ltp'])['ltp-glibc']
                self.assertEqual(result['cases'][0]['state'], 'reported-failure')
                self.assertEqual(result['observed_errors'][0]['output'], output)

    def test_combined_score_uses_original_nonlinear_ltp_formula(self):
        function = getattr(runner, 'original_score', None)
        self.assertTrue(callable(function), 'runner lacks joint upstream scoring')
        result = function({'rv': {'ltp-musl': [{'name': 'LTP', 'score': 100}]},
                           'la': {'ltp-musl': [{'name': 'LTP', 'score': 100}]}}, {})
        self.assertEqual(result['postwork_integer_score'], 35)
        self.assertIn('ltp-musl-rv', result['postwork_rank'])
        self.assertIn('ltp-musl-la', result['postwork_rank'])

    def test_finished_process_is_not_timeout_when_collection_is_delayed(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'serial.log'; entry = {}
            calls = 0
            def clock():
                nonlocal calls
                calls += 1
                if calls == 1: return 0.0
                if calls == 2: time.sleep(0.15)  # 模拟宿主调度停顿，实际child已自然结束。
                return 1.0
            with patch.object(runner.time, 'monotonic', side_effect=clock):
                runner.run_guests([('riscv', entry, [sys.executable, '-c',
                    'print("actual final output")'], log)], 0.1)
            self.assertEqual(entry['qemu_returncode'], 0)
            self.assertEqual(entry['exit_reason'], 'qemu-exit')
            self.assertEqual(log.read_text(), 'actual final output\n')

    def test_architecture_failure_and_full_output_remain_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            logs = [Path(temporary) / name for name in ('rv.log', 'la.log')]
            entries = [{}, {}]
            runner.run_guests([
                ('riscv', entries[0], [sys.executable, '-c', 'print("RV failed tail");raise SystemExit(7)'], logs[0]),
                ('loongarch', entries[1], [sys.executable, '-c', 'print("LA actual tail")'], logs[1])], 3)
            self.assertEqual([entry['qemu_returncode'] for entry in entries], [7, 0])
            self.assertEqual([log.read_text() for log in logs], ['RV failed tail\n', 'LA actual tail\n'])


if __name__ == '__main__':
    unittest.main()
