"""The optional LTP launcher must preserve upstream arguments and results."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class LtpLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'ltp'
        (self.root / 'bin').mkdir(parents=True)
        (self.root / 'testcases/bin').mkdir(parents=True)
        (self.root / 'runtest').mkdir()
        (self.root / 'runtest/controllers').write_text('controller controller.sh required-argument\n')
        (self.root / 'runtest/syscalls').write_text('next next01\n')
        (self.root / 'bin/ltp-pan').write_text('#!/bin/sh\nexit 0\n')
        (self.root / 'bin/ltp-pan').chmod(0o755)
        self.engine = self.root / 'runltp'
        self.engine.write_text('''#!/bin/sh
printf '%s\n' "$@" > "$LTP_CAPTURE/argv"
printf '%s\n' "$LTPROOT" "$CREATE_ENTRIES" "$PATH" > "$LTP_CAPTURE/env"
if read input; then echo 'unexpected interactive input'; exit 99; fi
while [ "$#" -gt 0 ]; do
    case "$1" in
        -l) printf 'controller CONF 32\nnext FAIL 2\n' > "$2";;
        -o) printf 'original case output\n' > "$2";;
        -C) printf 'next next01\n' > "$2";;
        -T) printf 'controller controller.sh required-argument\n' > "$2";;
    esac
    shift
done
exit "$LTP_ENGINE_STATUS"
''')
        self.engine.chmod(0o755)
        self.results = Path(self.temporary.name) / 'results'
        self.launch_count = 0

    def launch(self, status=0, suites='controllers,syscalls', pattern='controller\\|next'):
        self.launch_count += 1
        self.results = Path(self.temporary.name) / f'results-{self.launch_count}'
        environment = dict(os.environ, LTP_CAPTURE=self.temporary.name,
                           LTP_ENGINE_STATUS=str(status), LTPROOT='/wrong-root')
        return subprocess.run(['sh', str(HERE / 'ltp.sh'), str(self.root),
                               suites, pattern, str(self.results)],
                              input='input must not reach the test\n', text=True,
                              capture_output=True, env=environment, timeout=5)

    def test_upstream_selection_noninteractive_environment_and_raw_output(self):
        result = self.launch(status=1)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        argv = (Path(self.temporary.name) / 'argv').read_text().splitlines()
        self.assertEqual(argv[argv.index('-f') + 1], 'controllers,syscalls')
        self.assertEqual(argv[argv.index('-s') + 1], 'controller\\|next')
        root, creation, path = (Path(self.temporary.name) / 'env').read_text().splitlines()
        self.assertEqual(root, str(self.root))
        self.assertEqual(creation, '0')
        self.assertIn(str(self.root / 'testcases/bin'), path.split(':'))
        self.assertIn('controller CONF 32\nnext FAIL 2\n', result.stdout)
        self.assertIn('original case output\n', result.stdout)
        self.assertNotIn('unexpected interactive input', result.stdout)
        self.assertEqual((self.results / 'configurations').read_text(),
                         'controller controller.sh required-argument\n')

    def test_engine_status_is_preserved(self):
        for status in (0, 2, 32, 137):
            with self.subTest(status=status):
                self.assertEqual(self.launch(status=status).returncode, status)

    def test_no_pattern_does_not_filter_the_suite(self):
        result = self.launch(pattern='')
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = (Path(self.temporary.name) / 'argv').read_text().splitlines()
        self.assertNotIn('-s', argv)

    def test_missing_suite_is_a_setup_error(self):
        result = self.launch(suites='missing')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((Path(self.temporary.name) / 'argv').exists())


if __name__ == '__main__':
    unittest.main()
