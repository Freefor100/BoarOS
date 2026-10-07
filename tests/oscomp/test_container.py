"""Container submission must be a clean committed snapshot and pin its image."""
import importlib.util
import gzip
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class ContainerTests(unittest.TestCase):
    def module(self):
        path = HERE / 'official.py'
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

    def test_dirty_source_is_rejected_before_creating_submission(self):
        module = self.module(); repo, dest = self.repository()
        (repo / 'source').write_text('uncommitted input')
        with self.assertRaisesRegex(RuntimeError, 'committed'):
            module.snapshot_source(repo, dest)
        self.assertFalse(dest.exists())

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

    def test_lifecycle_requires_seen_owner_and_conservative_budget_evidence(self):
        module = self.module()
        record = {'samples': 0, 'budget_seconds': 10, 'architectures': {}}
        module.update_lifecycle(record, {'uptime': 100, 'processes': {'riscv': {
            'pid': 12, 'start_ticks': 9000, 'elapsed_seconds': 10.1}}})
        self.assertFalse(record['architectures']['riscv']['budget_observed'])
        self.assertNotIn('loongarch', record['architectures'])
        module.update_lifecycle(record, {'uptime': 101, 'processes': {'riscv': {
            'pid': 12, 'start_ticks': 9000, 'elapsed_seconds': 11.1}}})
        self.assertTrue(record['architectures']['riscv']['budget_observed'])
        with self.assertRaisesRegex(RuntimeError, 'owner changed'):
            module.update_lifecycle(record, {'uptime': 102, 'processes': {'riscv': {
                'pid': 13, 'start_ticks': 10100, 'elapsed_seconds': 1}}})
        module.update_lifecycle(record, {'uptime': 103, 'processes': {}})
        self.assertTrue(record['architectures']['riscv']['end_observed'])

    def test_container_entry_uses_upstream_parser_despite_local_run_module(self):
        self.module()
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        log = Path(temp.name) / 'serial.log'; log.write_text('')
        code = ('import sys;sys.path.insert(0,sys.argv[1]);import official;'
                'print(len(official.local.judge(__import__("pathlib").Path(sys.argv[2]),{})))')
        result = subprocess.run(['python3', '-B', '-c', code, str(HERE), str(log)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '22')

    def test_gzip_cache_rejects_changed_contents_and_permissions(self):
        module = self.module()
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        root = Path(temp.name); source = root / 'disk.img'
        source.write_bytes(b'fixed original image bytes')
        expected = hashlib.sha256(source.read_bytes()).hexdigest()
        cached = module.gzip_asset(source, expected, root / 'cache')
        self.assertEqual(gzip.decompress(cached.read_bytes()), b'fixed original image bytes')
        self.assertEqual(module.gzip_asset(source, expected, root / 'cache'), cached)
        cached.chmod(0o600)
        with self.assertRaisesRegex(RuntimeError, 'identity'):
            module.gzip_asset(source, expected, root / 'cache')
        cached.chmod(0o644); cached.write_bytes(gzip.compress(b'changed bytes'))
        with self.assertRaisesRegex(RuntimeError, 'identity'):
            module.gzip_asset(source, expected, root / 'cache')


if __name__ == '__main__':
    unittest.main()
