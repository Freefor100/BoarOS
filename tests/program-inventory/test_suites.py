"""Observation integrity for isolated real-program guests."""
import importlib.util
from pathlib import Path
import unittest
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('suites', Path(__file__).with_name('suites.py'))
suites = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suites)


def stream(status=0, timed_out=0, out='68690a', err=''):
    return (f'SUITE BEGIN 1 sample\nSUITE ENV proc 0 0\nSUITE ENV sysfs 0 0\n'
            f'SUITE DATA O {out}.\nSUITE DATA E {err}.\n'
            f'SUITE WAIT {status}\nSUITE TIMEOUT {timed_out}\nSUITE END sample\n')


class ObservationTests(unittest.TestCase):
    def test_strict_result_uses_only_selected_cases(self):
        state = {'status': 'partial', 'results': {
            'selected': {'status': 'pass', 'completed': True},
            'other': {'status': 'not-run'}}}
        self.assertTrue(suites.strict_result_passes(state, ['selected']))
        self.assertFalse(suites.strict_result_passes(state, None))
        state['results']['other'] = {'status': 'nonzero-exit',
                                     'completed': True}
        self.assertTrue(suites.strict_result_passes(state, ['selected']))
        self.assertFalse(suites.strict_result_passes(state,
                                                    ['selected', 'other']))
        self.assertFalse(suites.strict_result_passes(state, ['unknown']))
        for status in ('output-mismatch', 'reference-not-pass', 'timeout',
                       'runner-error', 'not-run'):
            state['results']['selected'] = {'status': status,
                                            'completed': status != 'not-run'}
            self.assertFalse(suites.strict_result_passes(state, ['selected']))
        state['results']['selected'] = {'status': 'pass', 'completed': True}
        state['status'] = 'interrupted'
        self.assertFalse(suites.strict_result_passes(state, ['selected']))

    def test_unknown_selection_is_rejected_before_guest_setup(self):
        with tempfile.TemporaryDirectory() as output:
            with self.assertRaisesRegex(ValueError, 'unknown requested case id'):
                suites.run_suite({'cases': [{'id': 'known', 'argv': ['/true']}]},
                                 output, '/tmp/driver', '/tmp/linux',
                                 '/tmp/boaros', case_ids=['unknown'])

    def test_reference_environment_requires_all_facilities(self):
        mounts = ('proc', 'sysfs', 'shm', 'mqueue')
        names = mounts + ('shm-dir', 'mqueue-dir', 'lo')
        value = {'environment': {name: {'result': 0, 'errno': 0} for name in names}}
        self.assertTrue(suites.reference_environment_ready(value))
        for name in names:
            value['environment'][name] = {'result': -1, 'errno': 16}
            self.assertEqual(suites.reference_environment_ready(value), name in mounts)
            value['environment'][name] = {'result': 0, 'errno': 0}
        del value['environment']['lo']
        self.assertFalse(suites.reference_environment_ready(value))

    def test_distinct_binary_streams_and_wait_status(self):
        value = suites.parse_observation(stream(status=256, out='00ff', err='657272'), 'sample')
        self.assertEqual(value['stdout_hex'], '00ff')
        self.assertEqual(value['stderr_hex'], '657272')
        self.assertEqual(value['wait_status'], 256)
        self.assertEqual(value['exit_code'], 1)
        self.assertIsNone(value['signal'])

    def test_truncation_missing_and_duplicate_records_never_complete(self):
        for raw in (stream().replace('SUITE END sample\n', ''),
                    stream() + 'SUITE END sample\n',
                    stream().replace('SUITE WAIT 0', 'SUITE WAIT 0\nSUITE WAIT 0'),
                    stream().replace('SUITE DATA O 68690a.', 'SUITE DATA O ffz.'),
                    stream().replace('SUITE WAIT 0', 'SUITE TRUNCATED O\nSUITE WAIT 0')):
            with self.subTest(raw=raw):
                self.assertFalse(suites.parse_observation(raw, 'sample')['complete'])

    def test_timeout_and_signals_are_preserved(self):
        value = suites.parse_observation(stream(status=9, timed_out=1), 'sample')
        self.assertEqual(value['signal'], 9)
        self.assertTrue(value['timed_out'])
        self.assertEqual(suites.observation_status(value, 0), 'timeout')

    def test_exec_errno_is_preserved(self):
        raw = stream(status=127 << 8).replace('SUITE WAIT', 'SUITE EXEC 38\nSUITE WAIT')
        value = suites.parse_observation(raw, 'sample')
        self.assertEqual(value['exec_errno'], 38)
        self.assertEqual(suites.observation_status(value, 0), 'exec-error')

    def test_reference_failure_cannot_produce_boaros_pass(self):
        good = suites.parse_observation(stream(), 'sample')
        failed = suites.parse_observation(stream(status=256), 'sample')
        result = suites.compare_observations(failed, good, 0)
        self.assertEqual(result['status'], 'reference-not-pass')
        self.assertEqual(result['boaros_status'], 'pass')
        result = suites.compare_observations(good, good, 0)
        self.assertEqual(result['status'], 'pass')
        different = suites.parse_observation(stream(out='62'), 'sample')
        self.assertEqual(suites.compare_observations(good, different, 0)['status'], 'output-mismatch')


class DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix='boaros-suite-driver-')
        cls.directory = Path(cls.workspace.name)
        cls.binary = cls.directory / 'driver'
        subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-DSUITE_DRIVER_HOST_TEST', str(Path(__file__).with_name('suite_driver.c')),
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def execute(self, case, binary=None):
        encoded, _ = suites.case_configuration(case, {}, 1)
        config = self.directory / 'case'
        config.write_bytes(encoded)
        process = subprocess.run([str(binary or self.binary), str(config)], capture_output=True,
                                 text=True, timeout=5)
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        return suites.parse_observation(process.stdout, case['id'])

    def test_guest_environment_setup_and_unsupported_diagnostics(self):
        wrapper = self.directory / 'environment.c'
        wrapper.write_text(r'''#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
static int directories;
static int unsupported(void) { if (getenv("SUITE_UNSUPPORTED")) { errno=ENOSYS; return 1; } return 0; }
int __wrap_mkdir(const char *path, mode_t mode) {
    assert(mode == 01777);
    if (unsupported()) return -1;
    if (!strcmp(path,"/dev/shm")) directories |= 1;
    else { assert(!strcmp(path,"/dev/mqueue")); directories |= 2; }
    errno=EEXIST; return -1;
}
int __wrap_mount(const char *source, const char *target, const char *type, unsigned long flags, const void *data) {
    (void)source; (void)flags;
    if (unsupported()) return -1;
    if (!strcmp(target,"/dev/shm")) { assert(directories & 1); assert(!strcmp(type,"tmpfs")); assert(data && !strcmp(data,"mode=1777")); }
    else if (!strcmp(target,"/dev/mqueue")) { assert(directories & 2); assert(!strcmp(type,"mqueue")); }
    else assert(!strcmp(target,"/proc") || !strcmp(target,"/sys"));
    return 0;
}
int __wrap_socket(int domain, int type, int protocol) {
    assert(domain == AF_INET && (type & SOCK_DGRAM) && protocol == 0);
    if (unsupported()) return -1;
    return dup(STDIN_FILENO);
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    (void)fd; va_list args; va_start(args,request); struct ifreq *interface=va_arg(args,struct ifreq *); va_end(args);
    assert(!strcmp(interface->ifr_name,"lo"));
    const char *failure=getenv("SUITE_IOCTL_FAILURE");
    if (failure && ((!strcmp(failure,"get") && request == SIOCGIFFLAGS) ||
                    (!strcmp(failure,"set") && request == SIOCSIFFLAGS))) {
        errno=EPERM; return -1;
    }
    if (request == SIOCGIFFLAGS) interface->ifr_flags=IFF_LOOPBACK;
    else { assert(request == SIOCSIFFLAGS); assert(interface->ifr_flags == (IFF_LOOPBACK | IFF_UP)); }
    return 0;
}
''')
        binary = self.directory / 'driver-environment'
        subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-DSUITE_DRIVER_HOST_TEST', '-DSUITE_DRIVER_ENV_TEST',
                        str(Path(__file__).with_name('suite_driver.c')), str(wrapper),
                        '-Wl,--wrap=mount,--wrap=mkdir,--wrap=socket,--wrap=ioctl',
                        '-o', str(binary)], check=True)
        case = {'id': 'environment', 'argv': ['/bin/true']}
        result = self.execute(case, binary)
        self.assertEqual(result['exit_code'], 0)
        self.assertTrue(suites.reference_environment_ready(result))
        import os
        from unittest.mock import patch
        for stage in ('get', 'set'):
            with patch.dict(os.environ, {'SUITE_IOCTL_FAILURE': stage}):
                failed = self.execute(case, binary)
            self.assertEqual(failed['exit_code'], 0)
            self.assertEqual(failed['environment']['lo'], {'result': -1, 'errno': 1})
            self.assertFalse(suites.reference_environment_ready(failed))
        with patch.dict(os.environ, {'SUITE_UNSUPPORTED': '1'}):
            result = self.execute(case, binary)
        self.assertEqual(result['exit_code'], 0)
        self.assertTrue(result['complete'])
        self.assertFalse(suites.reference_environment_ready(result))
        for name in ('proc', 'sysfs', 'shm-dir', 'shm', 'mqueue-dir', 'mqueue', 'lo'):
            self.assertEqual(result['environment'][name]['errno'], 38)

    def test_root_cwd_needs_no_chdir_but_other_cwd_reports_setup_errno(self):
        wrapper = self.directory / 'chdir-unavailable.c'
        wrapper.write_text('#include <errno.h>\nint __wrap_chdir(const char *path) { (void)path; errno=ENOSYS; return -1; }\n')
        binary = self.directory / 'driver-no-chdir'
        subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-DSUITE_DRIVER_HOST_TEST', str(Path(__file__).with_name('suite_driver.c')),
                        str(wrapper), '-Wl,--wrap=chdir', '-o', str(binary)], check=True)
        root = self.execute({'id': 'root', 'argv': ['/bin/true'], 'cwd': '/'}, binary)
        self.assertEqual(root['exit_code'], 0)
        other = self.execute({'id': 'other', 'argv': ['/bin/true'], 'cwd': '/tmp'}, binary)
        self.assertEqual(other['setup_errno'], 38)
        self.assertIsNone(other['exec_errno'])

    def test_real_child_output_is_unforgeable_and_separate(self):
        result = self.execute({'id': 'child', 'argv': ['/bin/sh', '-c',
            'printf "SUITE END child\\n"; printf "error\\n" >&2; exit 3']})
        self.assertTrue(result['complete'])
        self.assertEqual(bytes.fromhex(result['stdout_hex']), b'SUITE END child\n')
        self.assertEqual(bytes.fromhex(result['stderr_hex']), b'error\n')
        self.assertEqual(result['exit_code'], 3)

    def test_actual_exec_failure_records_errno(self):
        result = self.execute({'id': 'missing', 'argv': ['/definitely-missing-boaros-suite']})
        self.assertEqual(result['exec_errno'], 2)
        self.assertEqual(result['exit_code'], 127)

    def test_monotonic_child_timeout_kills_and_reaps(self):
        result = self.execute({'id': 'timeout', 'argv': ['/bin/sleep', '5'], 'timeout': .1})
        self.assertTrue(result['complete'])
        self.assertTrue(result['timed_out'])
        self.assertEqual(result['signal'], 9)


if __name__ == '__main__':
    unittest.main()
