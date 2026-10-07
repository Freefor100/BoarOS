"""Explicit shell entry preserves the original nested scripts and markers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class ScriptAdaptationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'original.sh'
        self.target = self.root / 'adapted.sh'
        self.original = ('# keep this comment about ./run-static.sh\n'
                         'echo GROUP-START\n./run-static.sh\n./run-dynamic.sh\necho GROUP-END\n')
        self.source.write_text(self.original)

    def adapt(self):
        return subprocess.run(['sh', str(HERE / 'libctest-hook.sh'),
                               str(self.source), str(self.target)], capture_output=True, text=True)

    def test_only_two_execution_lines_change_and_original_is_preserved(self):
        result = self.adapt()
        self.assertEqual(result.returncode, 0, result.stderr)
        adapted = self.target.read_text()
        for name in ('run-static.sh', 'run-dynamic.sh'):
            adapted = adapted.replace('"$BOAROS_CASE_SHELL" sh ./' + name, './' + name)
        self.assertEqual(adapted, self.original)
        self.assertEqual(self.source.read_text(), self.original)

    def test_native_scripts_continue_and_keep_their_output(self):
        result = self.adapt()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name, status in (('static', 7), ('dynamic', 9)):
            (self.root / ('run-' + name + '.sh')).write_text('echo ORIGINAL-' + name + '\nexit ' + str(status) + '\n')
        shell = self.root / 'multicall-shell'
        shell.write_text('#!/bin/sh\n[ "$1" = sh ] || exit 125\nshift\nexec /bin/sh "$@"\n')
        shell.chmod(0o755)
        result = subprocess.run(['sh', str(self.target)], cwd=self.root,
                                env={'BOAROS_CASE_SHELL': str(shell), 'PATH': '/usr/bin:/bin'},
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'GROUP-START\nORIGINAL-static\nORIGINAL-dynamic\nGROUP-END\n')

    def test_missing_duplicate_or_changed_execution_shape_is_rejected(self):
        for content in (self.original.replace('./run-static.sh\n', ''),
                        self.original + './run-dynamic.sh\n',
                        self.original.replace('./run-static.sh\n', './run-static.sh extra\n')):
            with self.subTest(content=content):
                self.source.write_text(content)
                result = self.adapt()
                self.assertEqual(result.returncode, 125)
                self.assertEqual(self.source.read_text(), content)


if __name__ == '__main__':
    unittest.main()
