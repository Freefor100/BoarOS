"""Cold CI inputs must fail atomically and host setup must preserve the job UID."""
import importlib.util
import os
import select
import signal
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ci_netns', Path(__file__).with_name('netns.py'))
netns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netns)


class NamespaceSetup(unittest.TestCase):
    def test_probe_timeout_stops_the_registered_process_group(self):
        ready_read, ready_write = os.pipe()
        with os.fdopen(ready_read, 'rb', buffering=0) as ready, \
             os.fdopen(ready_write, 'wb', buffering=0) as publish, \
             tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / 'tests/network-external.py'
            script.parent.mkdir()
            script.write_text('import os,time\nchild=os.fork()\nif child==0:\n time.sleep(30)\n os._exit(0)\n'
                              'os.write(int(os.environ["READY_FD"]),str(child).encode()+b"\\n")\ntime.sleep(30)\n')
            real_popen = subprocess.Popen
            processes = []
            registered = []
            def launch(argv, **kwargs):
                kwargs['env']['READY_FD'] = str(publish.fileno())
                process = real_popen(argv, pass_fds=(publish.fileno(),), **kwargs)
                processes.append(process)
                publish.close()
                if not select.select([ready], [], [], 10)[0]:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                    self.fail('fixture did not register its process group')
                self.assertGreater(int(ready.readline()), 0)
                registered.append(time.monotonic())
                return process
            try:
                with patch.object(netns, 'ROOT', root), patch.object(netns, 'PROBE_TIMEOUT', .05), \
                     patch.object(netns.subprocess, 'Popen', side_effect=launch):
                    result = netns.probe('/unused-unshare')
                self.assertLess(result.returncode, 0)
                self.assertIn('timed out', result.stderr)
                self.assertLess(time.monotonic() - registered[0], 2)
                # fork后父子都持有写端；EOF确认后代也归还了实际借用，不能只reap父进程。
                self.assertEqual(select.select([ready], [], [], 2)[0], [ready])
                self.assertEqual(ready.read(1), b'')
            finally:
                for process in processes:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                    process.stdout.close()
                    process.stderr.close()

    def test_unavailable_host_is_not_changed_without_explicit_ci_scope(self):
        for github in ('false', 'true'):
            with self.subTest(github=github), patch.dict(os.environ, {'GITHUB_ACTIONS': github}), \
                 patch.object(netns, 'probe', return_value=subprocess.CompletedProcess([], 1, '', 'uid_map denied')), \
                 patch.object(netns.subprocess, 'run') as command:
                with self.assertRaisesRegex(RuntimeError, 'host policy was not changed'):
                    netns.prepare(configure=False)
                command.assert_not_called()

    def test_scoped_profile_must_pass_tap_probe_and_is_removed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / 'host-unshare'
            original.write_bytes(b'exact host executable')
            restriction = root / 'restriction'
            restriction.write_text('1\n')
            probes = [subprocess.CompletedProcess([], 1, '', 'uid_map denied'),
                      subprocess.CompletedProcess([], 0, 'TAP PASS\n', '')]
            with patch.dict(os.environ, {'GITHUB_ACTIONS': 'true'}), \
                 patch.object(netns, 'DIRECTORY', root / 'private'), \
                 patch.object(netns, 'RESTRICTION', restriction), \
                 patch.object(netns.shutil, 'which', return_value=str(original)), \
                 patch.object(netns, 'probe', side_effect=probes), \
                 patch.object(netns.subprocess, 'run') as command:
                private = netns.prepare(configure=True)
                self.assertEqual(private.read_bytes(), original.read_bytes())
                policy = root / 'private/userns.profile'
                self.assertIn(str(private), policy.read_text())
                self.assertNotIn('/usr/bin/unshare', policy.read_text())
                self.assertEqual([c.args[0][2:4] for c in command.call_args_list], [['apparmor_parser', '-r']])
                netns.cleanup()
                self.assertFalse(policy.exists())
                self.assertEqual(command.call_args.args[0][2:4], ['apparmor_parser', '-R'])


if __name__ == '__main__':
    unittest.main()
