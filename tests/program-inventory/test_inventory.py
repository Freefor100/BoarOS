"""A shell's zero exit cannot hide failures or missing upstream cases."""
import unittest
from reports import validate_output


def observation(stdout):
    return {'stdout_hex': stdout.encode().hex(), 'stderr_hex': ''}


class UpstreamOutputTests(unittest.TestCase):
    def test_busybox_requires_complete_markers_and_every_result(self):
        case = {'output_contract': {'kind': 'busybox-script', 'expected_records': 2}}
        good = ('#### OS COMP TEST GROUP START busybox ####\n'
                'testcase busybox true success\ntestcase busybox false success\n'
                '#### OS COMP TEST GROUP END busybox ####\n')
        self.assertEqual(validate_output(case, observation(good))['status'], 'pass')
        if case['output_contract']['kind'] == 'busybox-script':
            prefixed = good.replace('testcase busybox true', '\x1b[H\x1b[Jtestcase busybox true')
            self.assertEqual(validate_output(case, observation(prefixed))['status'], 'pass')
        for broken in (good.replace('true success', 'true fail'),
                       good.replace('testcase busybox false success\n', ''),
                       good.replace('#### OS COMP TEST GROUP END busybox ####\n', '')):
            self.assertNotEqual(validate_output(case, observation(broken))['status'], 'pass')

    def test_libc_requires_ordered_complete_per_case_success(self):
        case = {'output_contract': {'kind': 'libc-runtest', 'entry': 'entry-static.exe',
                                    'cases': ['argv', 'env']}}
        def block(name):
            return f'========== START entry-static.exe {name} ==========\nPass!\n========== END entry-static.exe {name} ==========\n'
        good = block('argv') + block('env')
        self.assertEqual(validate_output(case, observation(good))['status'], 'pass')
        for broken in (block('argv'), block('env') + block('argv'),
                       good.replace('Pass!', 'FAIL test [internal]', 1), good + block('env')):
            self.assertNotEqual(validate_output(case, observation(broken))['status'], 'pass')

    def test_libc_rejects_nested_unmatched_and_malformed_markers(self):
        case = {'output_contract': {'kind': 'libc-runtest', 'entry': 'entry-static.exe',
                                    'cases': ['argv']}}
        start = '========== START entry-static.exe argv ==========\n'
        end = '========== END entry-static.exe argv ==========\n'
        good = start + 'Pass!\n' + end
        for broken in (start + good, good + end, end + good,
                       good + 'Pass!\n', start + 'Pass!\nPass!\n' + end,
                       start + 'Pass!\n' + end.replace('argv', 'env'),
                       start + '========== START malformed\nPass!\n' + end):
            with self.subTest(output=broken):
                self.assertNotEqual(validate_output(case, observation(broken))['status'], 'pass')

    def test_busybox_checks_exact_command_order_when_supplied(self):
        case = {'output_contract': {'kind': 'busybox-script', 'expected_records': 2,
                                    'commands': ['true', 'false']}}
        begin = '#### OS COMP TEST GROUP START busybox ####\n'
        end = '#### OS COMP TEST GROUP END busybox ####\n'
        def output(commands):
            return begin + ''.join('testcase busybox ' + command + ' success\n'
                                   for command in commands) + end
        self.assertEqual(validate_output(case, observation(output(['true', 'false'])))['status'], 'pass')
        for commands in (['true', 'true'], ['false', 'true'], ['true', 'echo unrelated']):
            self.assertNotEqual(validate_output(case, observation(output(commands)))['status'], 'pass')


if __name__ == '__main__':
    unittest.main()
