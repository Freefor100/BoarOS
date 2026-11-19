"""Check the container source snapshot and root disk setup."""
import importlib.util
import ast
import zipfile
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class ContainerTests(unittest.TestCase):
    def module(self):
        path = HERE / 'pre.py'
        self.assertTrue(path.is_file(), 'official container entry is missing')
        spec = importlib.util.spec_from_file_location('official_runner', path)
        result = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(result)
        return result

    def repository(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name) / 'repo'; root.mkdir()
        subprocess.run(['git', 'init', '-q', root], check=True)
        (root / '.gitignore').write_text('build/\n')
        source = root / 'source'; source.write_text('committed input\n'); source.chmod(0o755)
        subprocess.run(['git', '-C', root, 'add', '.'], check=True)
        subprocess.run(['git', '-C', root, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                        '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture'], check=True)
        return root, Path(temp.name) / 'submit'

    def test_snapshot_contains_committed_files_and_modes_without_build_cache(self):
        module = self.module(); repo, dest = self.repository()
        (repo / 'build').mkdir(); (repo / 'build/cached').write_text('host-only')
        identity = module.snapshot_source(repo, dest)
        self.assertEqual((dest / 'source').read_text(), 'committed input\n')
        self.assertEqual((dest / 'source').stat().st_mode & 0o777, 0o755)
        self.assertFalse((dest / 'build').exists())
        self.assertEqual(len(identity['commit']), 40)

    def test_snapshot_uses_the_commit_with_local_edits_present(self):
        module = self.module(); repo, dest = self.repository()
        source = repo / 'source'
        source.write_text('uncommitted input')
        module.snapshot_source(repo, dest)
        self.assertEqual((dest / 'source').read_text(), 'committed input\n')
        self.assertEqual(source.read_text(), 'uncommitted input')

    def test_container_tag_cannot_replace_pinned_digest(self):
        module = self.module()
        with self.assertRaisesRegex(ValueError, 'digest'):
            module.require_digest('zhouzhouyi/os-contest:20260510')
        self.assertEqual(module.require_digest('image@sha256:' + 'a' * 64), 'image@sha256:' + 'a' * 64)

    def test_original_compile_error_is_classified_before_serial_collection(self):
        module = self.module()
        self.assertEqual(module.harness_error_stage({'verdict': 'Compile Error', 'score': '0', 'rank': -1}),
                         'compilation')
        self.assertEqual(module.harness_error_stage({'verdict': 'Runtime Error'}), 'official-harness')
        self.assertIsNone(module.harness_error_stage({'verdict': 'Accpted'}))

    def test_complete_script_without_process_end_cannot_prove_natural_exit(self):
        module = self.module()
        serial = 'BOAROS-EVAL COMPLETE\n'
        self.assertEqual(module.classify_official_exit(serial, None), 'unknown-lifecycle')
        self.assertEqual(module.classify_official_exit(serial, {'budget_observed': True}),
                         'total-budget-timeout')

    def test_partial_script_without_budget_evidence_is_not_timeout(self):
        module = self.module()
        serial = 'BOAROS-EVAL ENTER ltp-musl\n'
        self.assertEqual(module.classify_official_exit(serial, None), 'unknown-lifecycle')
        self.assertEqual(module.classify_official_exit(serial, {'end_observed': True}), 'qemu-exit')

    def test_kernel_fatal_is_not_normal_exit_or_budget_timeout(self):
        module = self.module()
        for fatal in ('BoarOS: fatal trap scause=0xd sepc=0xffffffff80058472 stval=0x38',
                      'BoarOS: fatal LA kernel trap'):
            for evidence in (None, {'end_observed': True}, {'budget_observed': True}):
                with self.subTest(fatal=fatal, evidence=evidence):
                    self.assertEqual(module.classify_official_exit(fatal+'\n', evidence),
                                     'kernel-runtime-error')

    def test_baseline_requires_both_architectures_without_run_blockers(self):
        module = self.module()
        natural = {'exit_reason': 'qemu-exit', 'root_resources_verified': True}
        budget = {'exit_reason': 'total-budget-timeout', 'root_resources_verified': False}
        fatal = {'exit_reason': 'kernel-runtime-error', 'root_resources_verified': False}
        self.assertTrue(module.baseline_ready({'riscv': natural, 'loongarch': budget}))
        self.assertFalse(module.baseline_ready({'riscv': natural}))
        self.assertFalse(module.baseline_ready({'riscv': fatal, 'loongarch': budget}))
        self.assertFalse(module.baseline_ready({'riscv': natural, 'loongarch': {
            'exit_reason': 'unknown-lifecycle', 'root_resources_verified': False}}))

    def test_lifecycle_requires_seen_owner_and_conservative_budget_evidence(self):
        module = self.module()
        record = {'samples': 0, 'budget_seconds': 10, 'architectures': {}}
        module.update_lifecycle(record, {'uptime': 100, 'clock_ticks_per_second': 100,
            'processes': {'riscv': {
            'pid': 12, 'start_ticks': 9000, 'elapsed_seconds': 9.99}}})
        self.assertFalse(record['architectures']['riscv']['budget_observed'])
        self.assertNotIn('loongarch', record['architectures'])
        module.update_lifecycle(record, {'uptime': 101, 'clock_ticks_per_second': 100,
            'processes': {'riscv': {
            'pid': 12, 'start_ticks': 9000, 'elapsed_seconds': 10.94}}})
        self.assertTrue(record['architectures']['riscv']['budget_observed'])
        with self.assertRaisesRegex(RuntimeError, 'owner changed'):
            module.update_lifecycle(record, {'uptime': 102, 'processes': {'riscv': {
                'pid': 13, 'start_ticks': 10100, 'elapsed_seconds': 1}}})
        module.update_lifecycle(record, {'uptime': 103, 'processes': {}})
        self.assertTrue(record['architectures']['riscv']['end_observed'])

    def test_budget_reclassification_keeps_raw_evidence_and_tick_uncertainty(self):
        module = self.module()
        observed = {'pid': 12, 'start_ticks': 9000, 'elapsed_seconds': 10.94,
                    'budget_observed': False, 'end_observed': False}
        effective = module.budget_evidence(observed, 10, 100)
        self.assertTrue(effective['budget_observed'])
        self.assertFalse(observed['budget_observed'])
        self.assertFalse(effective['end_observed'])
        for elapsed in (9.99, 10.0, 10.005):
            self.assertFalse(module.budget_evidence(
                {**observed, 'elapsed_seconds': elapsed}, 10, 100)['budget_observed'])
        self.assertIsNone(module.budget_evidence(None, 10, 100))

    def test_container_entry_uses_upstream_parser_despite_local_run_module(self):
        self.module()
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        log = Path(temp.name) / 'serial.log'; log.write_text('')
        code = ('import sys;sys.path.insert(0,sys.argv[1]);import pre;'
                'print(len(pre.local.judge(__import__("pathlib").Path(sys.argv[2]),{})))')
        result = subprocess.run(['python3', '-B', '-c', code, str(HERE), str(log)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '22')

    def test_local_harness_stages_overlays_without_touching_the_judge(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'kernel.zip'
            module.package_harness(path)
            with zipfile.ZipFile(path) as archive:
                run = archive.read('run.py').decode()
                qemu = archive.read('run_qemu.py').decode().split('def run_qemu(job,', 1)[1]
                ast.parse(run)
                ast.parse(archive.read('run_qemu.py'))
                for arch in ('rv', 'la'):
                    self.assertIn(f'/coursegrader/sdcard/sdcard-{arch}.img sdcard-{arch}.qcow2', run)
                    self.assertIn(f'"sdcard-{arch}.qcow2", "os_serial_out_{arch}.txt"', run)
                self.assertNotIn('.img.gz', run + qemu)
                self.assertEqual(qemu.count('format=qcow2,id=x0'), 3)
                self.assertIn('file=disk.img,if=none,format=raw,id=x1', qemu)
                self.assertEqual(archive.read('postwork.py'), (module.REF / 'kernel/postwork.py').read_bytes())


if __name__ == '__main__':
    unittest.main()
