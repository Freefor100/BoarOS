"""The restoring process retains the actual flock after its controller dies."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest

from cost_measurement import acquire_measurement_lock

TESTS = str(Path(__file__).resolve().parent)


class GuardianTest(unittest.TestCase):
    def test_controller_sigkill_does_not_release_lock_during_restoration(self):
        import cost_policy_guard
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            lock = root / 'lock'
            policy = root / 'fake-policy'
            policy.write_text('powersave')
            resume_r, resume_w = os.pipe()
            helper = (f'import os,sys; from pathlib import Path; p=Path({str(policy)!r}); '
                      f'p.write_text("performance"); print("READY",flush=True); '
                      f'sys.stdin.read(); print("RESTORING",flush=True); '
                      f'os.read({resume_r},1); p.write_text("powersave"); print("RESTORED",flush=True)')
            guardian = (f'import os,sys; sys.path.insert(0,{TESTS!r}); '
                        f'from cost_policy_guard import serve_guardian; '
                        f'from cost_measurement import acquire_measurement_lock; '
                        f'fd=int(sys.argv[1]); lease=acquire_measurement_lock({str(lock)!r},fd); '
                        f'os.close(fd); sys.exit(serve_guardian(lease,'
                        f'command=[sys.executable,"-c",{helper!r}],pass_fds=({resume_r},)))')
            controller = (f'import sys,subprocess,signal; sys.path.insert(0,{TESTS!r}); '
                          f'from cost_measurement import acquire_measurement_lock; '
                          f'lease=acquire_measurement_lock({str(lock)!r}); '
                          f'p=subprocess.Popen([sys.executable,"-c",{guardian!r},str(lease.fileno())],'
                          f'stdin=subprocess.PIPE,pass_fds=(lease.fileno(),{resume_r})); '
                          f'print("PID",p.pid,flush=True); signal.pause()')
            parent = subprocess.Popen([sys.executable, '-c', controller], stdout=subprocess.PIPE,
                                      text=False, bufsize=0, pass_fds=(resume_r,))
            os.close(resume_r)
            guard_pid = None
            try:
                # Both lines establish real helper acquisition; their order is irrelevant.
                lines = []
                for _ in range(2):
                    self.assertTrue(select.select([parent.stdout], [], [], 10)[0])
                    lines.append(parent.stdout.readline().strip())
                self.assertIn(b'READY', lines)
                guard_pid = int(next(line.split()[1] for line in lines if line.startswith(b'PID ')))
                parent.kill()
                parent.wait(timeout=10)
                self.assertTrue(select.select([parent.stdout], [], [], 10)[0])
                self.assertEqual(parent.stdout.readline().strip(), b'RESTORING')
                self.assertEqual(policy.read_text(), 'performance')
                with self.assertRaises(BlockingIOError):
                    acquire_measurement_lock(lock)
                # Termination of the guardian must not abandon the still-restoring owner.
                os.kill(guard_pid, signal.SIGTERM)
                with self.assertRaises(BlockingIOError):
                    acquire_measurement_lock(lock)
                os.write(resume_w, b'R')
                self.assertTrue(select.select([parent.stdout], [], [], 10)[0])
                self.assertEqual(parent.stdout.readline().strip(), b'RESTORED')
                while True:
                    self.assertTrue(select.select([parent.stdout], [], [], 10)[0])
                    if not parent.stdout.readline():
                        break
                self.assertEqual(policy.read_text(), 'powersave')
                with acquire_measurement_lock(lock):
                    pass
            finally:
                try:
                    os.write(resume_w, b'R')
                except BrokenPipeError:
                    pass
                os.close(resume_w)
                if parent.poll() is None:
                    parent.kill()
                parent.wait()
                parent.stdout.close()
                if guard_pid:
                    try:
                        os.kill(guard_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_guardian_holds_lease_until_policy_process_exit(self):
        import cost_policy_guard
        with tempfile.TemporaryDirectory() as folder:
            lock = Path(folder) / 'lock'
            lease = acquire_measurement_lock(lock)
            with self.assertRaises(BlockingIOError):
                acquire_measurement_lock(lock)
            code = cost_policy_guard.serve_guardian(lease,
                command=[sys.executable, '-c', 'raise SystemExit(7)'])
            self.assertEqual(code, 7)
            with acquire_measurement_lock(lock):
                pass


if __name__ == '__main__':
    unittest.main()
