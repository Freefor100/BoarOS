"""Check the official script hook and the owned-child time limit."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class OfficialFlowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'case'
        subprocess.run(['cc', '-DCASE_HOST', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(HERE / 'case.c'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, command, limit='3'):
        return subprocess.run([str(self.binary), limit, '/bin/sh', '/bin/sh', '-c', command],
                              text=True, capture_output=True, timeout=6)

    def test_native_exit_and_output_are_preserved(self):
        result = self.run_case('printf "native output\\n"; exit 7')
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout, 'native output\n')

    def test_timeout_is_not_success_and_next_case_runs(self):
        result = self.run_case('while :; do :; done', limit='1')
        self.assertEqual(result.returncode, 124, result.stderr)
        self.assertIn('BOAROS-CASE TIMEOUT', result.stderr)
        self.assertIn('wait_status=', result.stderr)
        context = re.search(r'child_pid=(\d+) test_pgid=(\d+) deadline_elapsed_ms=(\d+) total_elapsed_ms=(\d+)', result.stderr)
        self.assertIsNotNone(context, result.stderr)
        child, group, deadline, total = map(int, context.groups())
        self.assertGreater(child, 0)
        self.assertGreater(group, 0)
        self.assertGreaterEqual(deadline, 1000)
        self.assertGreaterEqual(total, deadline)
        self.assertEqual(self.run_case('exit 0').returncode, 0)

    def test_signal_result_and_shell_text_fallback(self):
        self.assertEqual(self.run_case('kill -TERM $$').returncode, 143)
        script = Path(self.temp.name) / 'no-shebang'
        script.write_text('printf "shell fallback\\n"\nexit 9\n')
        script.chmod(0o755)
        result = subprocess.run([str(self.binary), '3', '/bin/sh', str(script)],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 9, result.stderr)
        self.assertEqual(result.stdout, 'shell fallback\n')

    def test_ignored_term_is_killed_and_reaped(self):
        result = self.run_case('trap "" TERM; while :; do :; done', limit='1')
        self.assertEqual(result.returncode, 124, result.stderr)
        self.assertIn('wait_status=9', result.stderr)

    def test_group_signal_cannot_kill_the_official_wrapper(self):
        # A separate session protects the host even when the unfixed helper kills PGID 0.
        result = subprocess.run(
            ['/bin/sh', '-c', '"$@" & job=$!; wait "$job"; exit $?', 'official-wrapper',
             str(self.binary), '3', '/bin/sh', '/bin/sh', '-c',
             'trap ":" USR1; kill -USR1 0; printf "native group survived\\n"; exit 7'],
            start_new_session=True, text=True, capture_output=True, timeout=6)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout, 'native group survived\n')

    def test_native_case_can_still_create_a_session(self):
        source = Path(self.temp.name) / 'setsid.c'
        binary = Path(self.temp.name) / 'setsid'
        source.write_text('#include <unistd.h>\nint main(void) { return setsid() < 0; }\n')
        subprocess.run(['cc', str(source), '-o', str(binary)], check=True)
        result = subprocess.run([str(self.binary), '3', '/bin/sh', str(binary)],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_background_descendant_is_stopped_with_its_case(self):
        result = self.run_case('(trap "" TERM; while :; do :; done) & echo "$!"; exit 0')
        self.assertEqual(result.returncode, 0, result.stderr)
        pid = int(result.stdout)
        stat = Path(f'/proc/{pid}/stat')
        if stat.exists():
            self.assertEqual(stat.read_text().rsplit(')', 1)[1].split()[0], 'Z')

    def test_hook_changes_only_the_official_execution_line(self):
        original = '#!/bin/bash\nfor file in ltp/testcases/bin/*; do\n    "$file"\n    ret=$?\n    echo "FAIL LTP CASE $(basename "$file") : $ret"\ndone\n'
        source = Path(self.temp.name) / 'official.sh'
        target = Path(self.temp.name) / 'adapted.sh'
        source.write_text(original)
        subprocess.run(['sh', str(HERE / 'ltp-hook.sh'), str(source), str(target)], check=True)
        hook = '    sh /tmp/boaros-ltp-case.sh /tmp/boaros-case /tmp/boaros-ltp-skips.tsv "$BOAROS_LTP_CASE_TIMEOUT" "$BOAROS_CASE_SHELL" "$file"'
        self.assertEqual(target.read_text().replace(hook, '    "$file"'), original)
        for malformed in (original.replace('    "$file"', '    echo "$file"'),
                          original + '    "$file"\n'):
            source.write_text(malformed)
            result = subprocess.run(['sh', str(HERE / 'ltp-hook.sh'), str(source), str(target)], capture_output=True)
            self.assertEqual(result.returncode, 125)

    def test_confirmed_controller_helper_is_skipped_without_success(self):
        program = Path(self.temp.name) / 'cgroup_fj_proc'
        program.write_text('#!/bin/sh\necho SHOULD-NOT-RUN\n')
        program.chmod(0o755)
        result = subprocess.run(['sh', str(HERE / 'ltp-case.sh'), str(self.binary),
            str(HERE / 'ltp-skips.tsv'), '3', '/bin/sh', str(program)],
            capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 125)
        self.assertEqual(result.stdout, '')
        self.assertIn('BOAROS-CASE SKIP', result.stderr)
        self.assertIn('cgroup_fj_proc', result.stderr)

    def test_unlisted_case_keeps_native_exit_and_output(self):
        program = Path(self.temp.name) / 'ordinary-test'
        program.write_text('#!/bin/sh\necho real-result\nexit 7\n')
        program.chmod(0o755)
        result = subprocess.run(['sh', str(HERE / 'ltp-case.sh'), str(self.binary),
            str(HERE / 'ltp-skips.tsv'), '3', '/bin/sh', str(program)],
            capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(result.stdout, 'real-result\n')


if __name__ == '__main__':
    unittest.main()
