#!/usr/bin/env python3
"""Host contracts for data identity, completion modes and I/O result accounting."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('io_budget', ROOT / 'tests/io-budget-experiment.py')
io = importlib.util.module_from_spec(spec); spec.loader.exec_module(io)


class Profiles(unittest.TestCase):
    def test_matrix_and_defaults(self):
        rows = io.profiles('all')
        self.assertEqual(len(rows), 20)
        self.assertEqual([r['name'] for r in rows if r['production_default']], ['ra0-wb1'])
        for bad in ('ra16-wb1', 'ra1-wb0', 'ra3-wb3'):
            with self.assertRaises(ValueError): io.profile(bad)

    def test_no_fake_cold_tmpfs(self):
        for bad in ('tmpfs:cold-read:cache:1:1M:4K', 'ext4:hot-read:fsync:1:1M:4K', 'ext4:read:cache:1:1M:4K'):
            with self.assertRaises(ValueError): io.parse_case(bad)
        for size in ('1M', '4M', '16M', '64M'):
            for request in ('1K', '4K', '64K'):
                io.parse_case(f'ext4:cold-read:cache:4:{size}:{request}')

    def test_smoke_is_not_promoted_to_performance(self):
        case = io.parse_case('ext4:cold-read:cache:1:1M:4K')
        identity = dict(cases=[case], variants={'current': {'kernel_sha256': 'K', 'management': 'unmanaged'}},
                        program_sha256='P', repeat=1, observe=False, smoke=True,
                        evidence_kind='functional-smoke-not-performance')
        row = dict(case=case, variant='current', repetition=1, kernel_sha256='K', program_sha256='P',
                   fixture_sha256='F', observation_enabled=False, status='passed', metrics={'bytes': 1048576})
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            io.summarize(work, identity, [row])
            result = json.loads((work / 'summary.json').read_text())
            self.assertEqual(result['groups'][0]['status'], 'smoke_passed')
            self.assertEqual(result['groups'][0]['metrics'], {})
            with self.assertRaises(RuntimeError): io.summarize(work, identity, [row | {'kernel_sha256': 'changed'}])
            with self.assertRaises(RuntimeError): io.summarize(work, identity, [row, row])


class Workload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(); cls.work = Path(cls.temp.name)
        cls.program = cls.work / 'data-path'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/workloads/io/data-path.c'), '-o', str(cls.program)], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def exercise(self, name, corrupt=False):
        case = io.parse_case(name)
        with tempfile.TemporaryDirectory(dir=self.work) as directory:
            work = Path(directory); data = work / 'data'
            if case['backend'] == 'ext4': io.seed(data, case)
            else: data.mkdir()
            if corrupt:
                with (data / 'file-0').open('r+b') as stream: stream.write(b'bad!')
            config = work / 'config'
            config.write_text('{backend} {operation} {completion} {files} {bytes} {request}\n'.format(**case))
            result = subprocess.run([str(self.program), str(config), str(data)], capture_output=True, text=True, timeout=20)
            if corrupt:
                self.assertNotEqual(result.returncode, 0); self.assertIn('IO FAIL', result.stderr); return
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            evidence = Path(str(config) + '.results').read_text()
            parsed = io.validate(evidence, case)
            self.assertEqual(parsed['metrics']['bytes'], case['files'] * case['bytes'])
            with self.assertRaises(RuntimeError): io.validate(evidence.replace('complete=1', 'complete=0', 1), case)
            with self.assertRaises(RuntimeError): io.validate(evidence.replace('sync_calls=0', 'sync_calls=9', 1) if 'sync_calls=0' in evidence else evidence.replace('sync_calls=1', 'sync_calls=9', 1), case)
            return parsed

    def test_ext4_workload_logic(self):
        # Host files test program semantics; real filesystem identities are QEMU evidence.
        for request in ('1K', '4K', '64K'):
            for operation, completion in (('append', 'cache'), ('overwrite', 'fsync'), ('cold-read', 'cache'), ('hot-read', 'cache')):
                with self.subTest(request=request, operation=operation):
                    self.exercise(f'ext4:{operation}:{completion}:1:1M:{request}')
        self.exercise('ext4:append:fdatasync:4:1M:4K')

    def test_memory_setup_logic(self):
        # No host mount is performed; actual tmpfs mount is exercised in guest smoke.
        self.exercise('tmpfs:append:cache:1:1M:4K')
        self.exercise('tmpfs:read:cache:1:1M:4K')
        self.exercise('tmpfs:hot-read:cache:4:1M:64K')

    def test_actual_content_corruption_is_rejected(self):
        self.exercise('ext4:cold-read:cache:1:1M:4K', corrupt=True)


if __name__ == '__main__': unittest.main()
