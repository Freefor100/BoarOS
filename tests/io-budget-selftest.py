#!/usr/bin/env python3
"""Host contracts for data identity, completion modes and I/O result accounting."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('io_budget', ROOT / 'tests/io-budget-experiment.py')
io = importlib.util.module_from_spec(spec); spec.loader.exec_module(io)


class Profiles(unittest.TestCase):
    def test_matrix_and_defaults(self):
        rows = io.profiles('all')
        self.assertEqual(len(rows), 42)
        self.assertEqual([r['name'] for r in rows if r['production_default']], ['ra16-wb32'])
        for bad in ('ra64-wb1', 'ra1-wb0', 'ra3-wb3'):
            with self.assertRaises(ValueError): io.profile(bad)

    def test_no_fake_cold_tmpfs(self):
        for bad in ('tmpfs:cold-read:cache:1:1M:4K', 'ext4:hot-read:fsync:1:1M:4K', 'ext4:read:cache:1:1M:4K'):
            with self.assertRaises(ValueError): io.parse_case(bad)
        for size in ('1M', '4M', '16M', '64M'):
            for request in ('1K', '4K', '64K'):
                io.parse_case(f'ext4:cold-read:cache:4:{size}:{request}')
        for bad in ('tmpfs:mixed-read:fsync:4:1M:4K', 'ext4:mixed-read:cache:4:1M:4K',
                    'ext4:mixed-read:fdatasync:4:1M:4K', 'ext4:mixed-read:fsync:1:1M:4K'):
            with self.assertRaises(ValueError): io.parse_case(bad)

    def test_kernel_heap_peak_records(self):
        self.assertEqual(io.kernel_heap_peak(' root-heap-peak-pages=3\n'),12288)
        for raw in ('',' root-heap-peak-pages=3\n root-heap-peak-pages=4\n'):
            with self.assertRaises(RuntimeError): io.kernel_heap_peak(raw)

    def test_invalid_host_cpu_selection_is_rejected_before_build(self):
        result=subprocess.run(['python3','-B',str(ROOT/'tests/io-budget-experiment.py'),
            'smoke','--variant','current=missing-kernel','--cpus','2',
            '--host-vcpu-cpus','0'],capture_output=True,text=True)
        self.assertEqual(result.returncode,2)
        self.assertIn('one per guest CPU',result.stderr)

    def test_seed_is_part_of_matched_input(self):
        case=io.parse_case('ext4:hot-read:cache:1:1M:4K')
        identity=dict(cases=[case],variants={'current':{'kernel_sha256':'K','management':'unmanaged'}},
            program_sha256='P',repeat=1,observe=False,smoke=True,qemu_seed=123,
            evidence_kind='functional-smoke-not-performance')
        row=dict(case=case,variant='current',repetition=1,kernel_sha256='K',program_sha256='P',
            fixture_sha256='F',observation_enabled=False,status='passed',metrics={},qemu_seed=123,
            command=['qemu','-seed','123'])
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory);io.summarize(work,identity,[row])
            for altered in (row|{'qemu_seed':124},row|{'command':['qemu','-seed','124']},
                row|{'command':['qemu']},row|{'command':['qemu','-seed','123','-seed','123']}):
                with self.assertRaises(ValueError):io.summarize(work,identity,[altered])

    def test_qemu_seed_is_fixed_across_variants_and_repetitions(self):
        args=SimpleNamespace(qemu='qemu',firmware='firmware',ram='1G',cpus=2,transport='modern',qemu_seed=123)
        for _ in range(5):
            for name in ('before','final','control'):
                command=io.qemu_command(Path(name),Path('disk'),name,args)
                self.assertEqual(command.count('-seed'),1)
                self.assertEqual(command[command.index('-seed')+1],'123')
        args.qemu_seed=None
        self.assertNotIn('-seed',io.qemu_command(Path('kernel'),Path('disk'),'current',args))

    def test_invalid_qemu_seed_is_rejected_before_build(self):
        for seed in ('-1','0x10000000000000000'):
            result=subprocess.run(['python3','-B',str(ROOT/'tests/io-budget-experiment.py'),
                'smoke','--variant','current=missing-kernel','--qemu-seed',seed],capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('fit uint64',result.stderr)

    def test_official_platform_rejects_fixture_seed_before_launch(self):
        result=subprocess.run(['python3','-B',str(ROOT/'tests/cost-riscv.py'),
            '--platform-config','official','--qemu-seed','123'],capture_output=True,text=True)
        self.assertEqual(result.returncode,2)
        self.assertIn('only to fixture measurements',result.stderr)

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
            self.assertEqual(parsed['metrics']['bytes'], 0 if case['operation']=='range-modify' else case['files'] * case['bytes'])
            for kind in ('SAMPLE','AFFINITY','RESOURCE'):
                incomplete=''.join(line for line in evidence.splitlines(keepends=True) if not line.startswith('IO '+kind+' '))
                with self.assertRaises(RuntimeError): io.validate(incomplete,case)
            with self.assertRaises(RuntimeError): io.validate(evidence.replace('complete=1', 'complete=0', 1), case)
            for replacement in ('complete=1junk', 'complete=0 complete=1'):
                with self.subTest(damaged_completion=replacement), self.assertRaises(RuntimeError):
                    io.validate(evidence.replace('complete=1', replacement, 1), case)
            with self.assertRaises(RuntimeError): io.validate(evidence.replace('sync_calls=0', 'sync_calls=9', 1) if 'sync_calls=0' in evidence else evidence.replace('sync_calls=1', 'sync_calls=9', 1), case)
            return parsed

    def test_request_latency_and_affinity(self):
        result=self.exercise('ext4:cold-read:cache:1:1M:64K')
        self.assertEqual(result['latencies']['io']['samples'],16)
        self.assertGreaterEqual(result['latencies']['io']['p95_ns'],result['latencies']['io']['p50_ns'])
        self.assertEqual(len(result['affinity']),1)
        self.assertEqual(result['affinity'][0]['role'],0)
        self.assertGreater(result['resources']['self_maxrss_kib'],0)

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

    def test_two_fixed_work_appenders(self):
        result=self.exercise('ext4:parallel-append:fsync:2:1M:4K')
        self.assertEqual(len(result['files']),2)
        self.assertEqual(result['metrics']['sync_calls'],2)
        self.assertEqual(result['latencies']['io']['samples'],512)
        for bad in ('tmpfs:parallel-append:fsync:2:1M:4K','ext4:parallel-append:cache:2:1M:4K',
                    'ext4:append:fsync:2:1M:4K'):
            with self.assertRaises(ValueError):io.parse_case(bad)

    def test_four_actors_use_every_available_cpu_with_fixed_work(self):
        import os
        available=sorted(os.sched_getaffinity(0))
        for count in (1,2,4):
            if len(available)<count: continue
            cpus=available[:count]
            cases=('ext4:parallel-append:fsync:4:1M:4K',
                   'ext4:periodic-sync:fsync:4:1M:4K',
                   'ext4:active-mixed:fsync:4:16M:4K',
                   'ext4:mmap-write:fsync:4:4M:1K')
            for name in cases:
                with self.subTest(cpus=cpus,case=name), tempfile.TemporaryDirectory(dir=self.work) as directory:
                    case=io.parse_case(name);work=Path(directory);io.seed(work/'data',case)
                    config=work/'config';config.write_text('{backend} {operation} {completion} {files} {bytes} {request} 120\n'.format(**case))
                    result=subprocess.run(['taskset','-c',','.join(map(str,cpus)),str(self.program),str(config),str(work/'data')],
                                          capture_output=True,text=True,timeout=125)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    parsed=io.validate(Path(str(config)+'.results').read_text(),case)
                    self.assertEqual({v['role']:v['cpu'] for v in parsed['affinity']},{i:cpus[i%count] for i in range(4)})
                    expected=24 if case['operation']=='active-mixed' else 16 if case['operation']=='mmap-write' else 4
                    self.assertEqual(parsed['metrics']['bytes'],expected*1048576)

    def test_fixed_range_work_and_preserved_prefix(self):
        result=self.exercise('ext4:range-modify:fsync:1:1M:64K')
        self.assertEqual(result['metrics']['range_operations'],64)
        self.assertEqual(result['metrics']['bytes'],0)
        self.assertEqual(result['latencies']['io']['samples'],16)
        for bad in ('tmpfs:range-modify:fsync:1:1M:64K','ext4:range-modify:cache:1:1M:64K',
                    'ext4:range-modify:fsync:4:1M:64K'):
            with self.assertRaises(ValueError):io.parse_case(bad)

    def test_mixed_read_and_close_intervals(self):
        case = io.parse_case('ext4:mixed-read:fsync:4:1M:4K')
        with tempfile.TemporaryDirectory(dir=self.work) as directory:
            work = Path(directory)
            io.seed(work / 'data', case)
            self.assertEqual((work / 'data/file-0').stat().st_size, 0)
            config = work / 'config'
            config.write_text('{backend} {operation} {completion} {files} {bytes} {request}\n'.format(**case))
            result = subprocess.run([str(self.program), str(config), str(work / 'data')],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            text = Path(str(config) + '.results').read_text()
            parsed = io.validate(text, case)
            self.assertEqual(parsed['metrics']['bytes'], case['bytes'] * 2)
            self.assertEqual(parsed['latencies']['read']['all']['samples'], 512)
            self.assertEqual(parsed['latencies']['close']['all']['samples'], 256)
            self.assertEqual(parsed['latencies']['open']['samples'], 255)
            self.assertGreater(parsed['metrics']['sync_ns'], 0)
            lines = text.splitlines(keepends=True)
            removed = False
            truncated = []
            for line in lines:
                if not removed and line.startswith('IO MIXED_SAMPLE '):
                    removed = True
                else:
                    truncated.append(line)
            with self.assertRaises(RuntimeError): io.validate(''.join(truncated), case)
            with self.assertRaises(RuntimeError): io.validate(text.replace('complete=1', 'complete=0', 1), case)
            altered = text.replace('id=1 sequence=0 ', 'id=9 sequence=0 ', 1)
            with self.assertRaises(RuntimeError): io.validate(altered, case)

    def exercise_policy(self, name):
        case=io.parse_case(name)
        with tempfile.TemporaryDirectory(dir=self.work) as directory:
            work=Path(directory);io.seed(work/'data',case);config=work/'config'
            config.write_text('{backend} {operation} {completion} {files} {bytes} {request} 120\n'.format(**case))
            result=subprocess.run([str(self.program),str(config),str(work/'data')],capture_output=True,text=True,timeout=125)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            text=Path(str(config)+'.results').read_text();parsed=io.validate(text,case)
            for prefix in ('POLICY_FILE','POLICY_SAMPLE','AFFINITY','RESOURCE'):
                damaged=''.join(line for line in text.splitlines(keepends=True) if not line.startswith('IO '+prefix+' '))
                with self.assertRaises(RuntimeError):io.validate(damaged,case)
            with self.assertRaises(RuntimeError):io.validate(text.replace('complete=1','complete=0',1),case)
            if case['operation'] in ('periodic-sync','active-mixed'):
                damaged=''.join(line for line in text.splitlines(keepends=True) if not line.startswith('IO POLICY_ROUND '))
                with self.assertRaises(RuntimeError):io.validate(damaged,case)
            return parsed

    def test_target_page_vectors(self):
        for count in (1,4,64):
            for temperature in ('hot','cold'):
                for layout in ('same','split'):
                    with self.subTest(count=count,temperature=temperature,layout=layout):
                        result=self.exercise_policy(f'ext4:writev-{count}-{temperature}-{layout}:fsync:1:16M:4K')
                        self.assertEqual(result['latencies']['write']['samples'],4096)

    def test_synchronous_write_modes(self):
        for completion in ('osync','odsync','fsync16','fdatasync16'):
            result=self.exercise_policy(f'ext4:sync-write:{completion}:1:4M:4K')
            self.assertEqual(result['latencies']['write']['samples'],1024)
            self.assertEqual(result['metrics']['sync_calls'],64 if completion.endswith('16') else 0)

    def test_hot_and_sparse_fixed_updates(self):
        result=self.exercise_policy('ext4:hot-write:fsync:1:16M:4K')
        self.assertEqual((result['metrics']['bytes'],result['metrics']['distinct_bytes']),(16*1048576,1048576))
        result=self.exercise_policy('ext4:sparse-write:fsync:1:4M:1K')
        self.assertEqual((result['metrics']['bytes'],result['metrics']['distinct_bytes']),(32*1048576,2*1048576))

    def test_four_writer_ready_fsync_done(self):
        result=self.exercise_policy('ext4:periodic-sync:fsync:4:1M:4K')
        self.assertEqual((result['metrics']['bytes'],result['metrics']['sync_calls'],len(result['rounds'])),(4*1048576,64,64))

    def test_true_mixed_writer_and_readers(self):
        result=self.exercise_policy('ext4:active-mixed:fsync:4:16M:4K')
        self.assertEqual(result['metrics']['writer_bytes'],16*1048576)
        self.assertEqual(result['latencies']['read']['samples'],2048)
        self.assertEqual(result['latencies']['open']['samples'],256)
        self.assertEqual(result['latencies']['close']['samples'],256)
        self.assertEqual(result['metrics']['sync_calls'],17)

    def test_shared_mmap_and_matching_pwrite_work(self):
        for operation in ('mmap-write','pwrite-pair'):
            result=self.exercise_policy(f'ext4:{operation}:fsync:2:4M:1K')
            self.assertEqual(result['metrics']['bytes'],16*1048576)
            self.assertEqual(result['latencies']['write']['samples'],16384)

    def test_complete_six_second_schedules(self):
        for operation,size in (('age-burst','4M'),('age-hot','16M')):
            result=self.exercise_policy(f'ext4:{operation}:fsync:1:{size}:4K')
            self.assertGreaterEqual(result['metrics']['scheduled_window_ns'],6000000000)
            self.assertGreater(result['metrics']['final_sync_ns'],0)

    def test_one_gib_stream_and_input_limits(self):
        result=self.exercise_policy('ext4:stream-write:fsync:1:1G:64K')
        self.assertEqual(result['metrics']['bytes'],1073741824)
        self.assertEqual(len(result['progress']),64)
        for bad in ('ext4:stream-write:fsync:1:16M:64K','ext4:stream-write:cache:1:1G:64K',
                    'ext4:sync-write:fsync:1:4M:4K','ext4:writev-65-hot-same:fsync:1:16M:4K'):
            with self.assertRaises(ValueError):io.parse_case(bad)

    def test_two_paths_on_one_backing_device_are_rejected(self):
        case=io.parse_case('ext4:domain-busy:fsync:2:512M:64K')
        with tempfile.TemporaryDirectory(dir=self.work) as directory:
            work=Path(directory);io.seed(work/'data',case);io.seed(work/'second',case);config=work/'config'
            config.write_text('{backend} {operation} {completion} {files} {bytes} {request} 120\n'.format(**case))
            result=subprocess.run([str(self.program),str(config),str(work/'data'),str(work/'second')],capture_output=True,text=True,timeout=5)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('st_dev',result.stderr)

    def test_mixed_latency_percentile_and_absent_overlap(self):
        self.assertEqual(io.latency_metrics([]), dict(samples=0, p50_ns=None, p95_ns=None, p99_ns=None, maximum_ns=None))
        self.assertEqual(io.latency_metrics(range(1, 101)), dict(samples=100, p50_ns=50, p95_ns=95, p99_ns=99, maximum_ns=100))


if __name__ == '__main__': unittest.main()
