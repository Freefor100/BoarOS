"""Report failures and combined scores from the real fixed upstream judge."""
import importlib.util
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

    def test_skip_and_timeout_remain_visible_in_a_completed_wrapper(self):
        text = ('BOAROS-EVAL ENTER ltp-musl\n#### OS COMP TEST GROUP START ltp-musl ####\n'
                'BOAROS-CASE SKIP command=x reason=controller\n'
                'BOAROS-CASE TIMEOUT-END wait_status=9\n'
                '#### OS COMP TEST GROUP END ltp-musl ####\nBOAROS-EVAL EXIT ltp-musl status=0\n')
        result = self.states(text, ['ltp'])['ltp-musl']
        self.assertEqual(result['state'], 'completed')
        self.assertEqual(result['supervision']['skipped'], 1)
        self.assertEqual(result['supervision']['timed_out'], 1)

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


if __name__ == '__main__':
    unittest.main()
