import importlib.util
from pathlib import Path
import sys
import unittest
import tempfile
sys.path.insert(0,str(Path(__file__).parent))
spec=importlib.util.spec_from_file_location('user_smp_runner',Path(__file__).with_name('user.py'))
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
qemu_spec=importlib.util.spec_from_file_location('user_qemu',Path(__file__).with_name('qemu.py'))
qemu=importlib.util.module_from_spec(qemu_spec);qemu_spec.loader.exec_module(qemu)
from unittest.mock import Mock,patch

class UserReport(unittest.TestCase):
    def test_execution_budget_is_bounded_and_explicit(self):
        with patch.dict(qemu.os.environ, {'BOAROS_SMP_TIMEOUT':'25'}):
            self.assertEqual(qemu.execution_timeout(),25)
        for value in ('0','181','nan','inf'):
            with patch.dict(qemu.os.environ, {'BOAROS_SMP_TIMEOUT':value}),self.assertRaises(ValueError):
                qemu.execution_timeout()
    def valid(self,cpus=2,dynamic=False):
        cases=runner.CASES+((runner.DYNAMIC_CASE,) if dynamic else ())
        lines=[f'USER-SMP BEGIN cpus={cpus}',*[f'USER-SMP CASE {name} OK' for name in cases],f'USER-SMP END {len(cases)}',
            'BoarOS: PID 1 exited status=0x2a pages=0x20 heap-live=0x0; shutting down']
        for hart in range(cpus):
            lines.append(f'BoarOS: USER SMP hart=0x{hart:x} traps=0x20 returns=0x20 local-irqs=0x2 syscalls=0x{15 if hart==0 else 7:x}')
        return '\n'.join(lines)
    def test_exact_stream_and_real_harts(self):
        for cpus in (1,2):self.assertEqual(len(runner.validate(self.valid(cpus),cpus,True)),len(runner.CASES)+2)
    def test_dynamic_profile_requires_the_dso_tls_observation(self):
        self.assertEqual(len(runner.validate(self.valid(dynamic=True),2,True,dynamic=True)),len(runner.CASES)+3)
        with self.assertRaises(RuntimeError):runner.validate(self.valid(),2,True,dynamic=True)
    def test_missing_duplicate_wrong_profile_failure_and_owner(self):
        text=self.valid()
        for invalid in (text.replace(f'USER-SMP END {len(runner.CASES)}',''),text+f'\nUSER-SMP END {len(runner.CASES)}',
            text.replace('cpus=2','cpus=1'),text.replace('CASE pthread-cancel','FAIL pthread-cancel'),
            text.replace('heap-live=0x0','heap-live=0x1'),text.replace('hart=0x1','hart=0x0'),
            text.replace('traps=0x20','traps=0x0'),text.replace('local-irqs=0x2','local-irqs=0x0')):
            with self.subTest(invalid=invalid),self.assertRaises(RuntimeError):runner.validate(invalid,2,True)
    def test_runs_are_fresh_and_preserved_outputs_are_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            base=Path(temp)/'runs'
            first=runner.create_run_directory(None,base)
            second=runner.create_run_directory(None,base)
            self.assertNotEqual(first,second)
            evidence=first/'report.json';evidence.write_text('original owner evidence')
            with self.assertRaises(RuntimeError):runner.create_run_directory(first,base)
            self.assertEqual(evidence.read_text(),'original owner evidence')
            explicit=Path(temp)/'explicit'
            self.assertEqual(runner.create_run_directory(explicit,base),explicit.resolve())
    def test_cpu1_must_dispatch_local_syscalls(self):
        with self.assertRaises(RuntimeError):runner.validate(self.valid().replace('syscalls=0x7','syscalls=0x0'),2,True)
    def test_exiting_proc_thread_is_not_a_runner_failure(self):
        good=Mock();good.read_text.return_value='CPU 1/TCG\n'
        vanished=Mock();vanished.read_text.side_effect=ProcessLookupError(3,'No such process')
        with patch.object(qemu.Path,'glob',return_value=[vanished,good]):
            self.assertEqual(qemu.observe_threads(123),{'CPU 1/TCG'})
    def test_thread_permission_error_remains_visible(self):
        denied=Mock();denied.read_text.side_effect=PermissionError(13,'Permission denied')
        with patch.object(qemu.Path,'glob',return_value=[denied]),self.assertRaises(PermissionError):qemu.observe_threads(123)

if __name__=='__main__':unittest.main()
