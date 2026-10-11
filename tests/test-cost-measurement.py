"""Exercise the global COST lock across real subprocess file descriptions."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    from cost_measurement import acquire_measurement_lock
except ModuleNotFoundError as error:
    if error.name != 'cost_measurement':
        raise
    acquire_measurement_lock = None


CHILD = '''
import sys
from cost_measurement import acquire_measurement_lock
path, mode, fd = sys.argv[1:]
try:
    lock = acquire_measurement_lock(path, None if fd == '-' else int(fd))
except (ValueError, OSError):
    if mode != 'reject':
        raise
else:
    lock.close()
    if mode == 'reject':
        raise AssertionError('unowned measurement lock accepted')
    if mode == 'fail-after-close':
        raise RuntimeError('intentional child failure after lease close')
'''


class MeasurementLockTest(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(acquire_measurement_lock,
                             'global measurement lock helper is missing')
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'measurement.lock'

    def child(self, mode, lock=None, path=None):
        descriptors = () if lock is None else (lock.fileno(),)
        environment = dict(os.environ,
                           PYTHONPATH=str(Path(__file__).resolve().parent))
        return subprocess.run(
            [sys.executable, '-B', '-c', CHILD, str(path or self.path), mode,
             '-' if lock is None else str(lock.fileno())],
            pass_fds=descriptors, capture_output=True, text=True,
            env=environment, timeout=10)

    def assert_held(self):
        with self.path.open('a') as probe:
            with self.assertRaises(BlockingIOError):
                fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def assert_available(self):
        with self.path.open('a') as probe:
            fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_standalone_lock_excludes_another_process_until_close(self):
        with acquire_measurement_lock(self.path):
            result = self.child('reject')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assert_held()
        self.assert_available()

    def test_inherited_lock_close_retains_parent_exclusion(self):
        with acquire_measurement_lock(self.path) as parent:
            result = self.child('accept', parent)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(parent.closed)
            self.assert_held()
        self.assert_available()

    def test_child_failure_after_close_retains_parent_exclusion(self):
        with acquire_measurement_lock(self.path) as parent:
            result = self.child('fail-after-close', parent)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('intentional child failure', result.stderr)
            self.assert_held()
        self.assert_available()

    def test_unlocked_inherited_description_is_rejected(self):
        with self.path.open('a') as unlocked:
            result = self.child('reject', unlocked)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(unlocked.closed)
            self.assert_available()

    def test_different_file_descriptor_is_rejected_without_unlocking(self):
        other = self.path.with_name('other.lock')
        with acquire_measurement_lock(self.path):
            with acquire_measurement_lock(other) as wrong:
                result = self.child('reject', wrong)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(wrong.closed)
                self.assert_held()
                with other.open('a') as probe:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_unrelated_owner_does_not_validate_an_unlocked_description(self):
        with acquire_measurement_lock(self.path):
            with self.path.open('a') as unrelated:
                result = self.child('reject', unrelated)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(unrelated.closed)
                self.assert_held()

    def test_replaced_lock_path_rejects_old_inode(self):
        with acquire_measurement_lock(self.path) as parent:
            replacement = self.path.with_name('replacement')
            replacement.write_text('')
            replacement.replace(self.path)
            result = self.child('reject', parent)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(parent.closed)
            self.assert_available()

    def test_nonregular_descriptor_is_rejected_without_closing_it(self):
        self.path.write_text('')
        reader, writer = os.pipe()
        try:
            with self.assertRaises(ValueError):
                acquire_measurement_lock(self.path, reader)
            os.fstat(reader)
        finally:
            os.close(reader)
            os.close(writer)

    def test_invalid_descriptor_is_rejected(self):
        self.path.write_text('')
        for descriptor in (-1, True, '3'):
            with self.subTest(descriptor=descriptor):
                with self.assertRaises(ValueError):
                    acquire_measurement_lock(self.path, descriptor)
        self.assert_available()


if __name__ == '__main__':
    unittest.main()
