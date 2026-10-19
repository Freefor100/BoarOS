"""Complete functional streams and independent controlled-owner evidence."""
import importlib.util
import tempfile
from pathlib import Path
import unittest

class CompositionReports(unittest.TestCase):
    def checker(self):
        spec=importlib.util.spec_from_file_location('composition',Path(__file__).with_name('composition.py'))
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        return module
    def test_boot_output_must_finish_before_user_records(self):
        module=self.checker()
        banner='BoarOS: timer frequency=0x989680 tick-hz=0x64 period=0x186a0\n'
        user='COMPOSITION CASE concurrent-first-fault OK\n'
        module.validate_boot_output(banner+user)
        for text in (user,banner+banner+user,user+banner,
                     banner.replace(' tick-hz',user+' tick-hz')):
            with self.subTest(text=text),self.assertRaises(RuntimeError):
                module.validate_boot_output(text)
    def test_shutdown_drains_the_entire_user_tail_before_diagnostics(self):
        module=self.checker()
        tail='COMPOSITION shutdown-tail '+'x'*358+'\n'
        final=' root-heap-peak-pages=131\n'
        module.validate_shutdown_output(tail+final)
        for text in (final,tail,tail+tail+final,final+tail,
                     tail[:128]+final+tail[128:]):
            with self.subTest(text=text),self.assertRaises(RuntimeError):
                module.validate_shutdown_output(text)
    def test_controlled_owner_evidence_is_not_a_functional_marker(self):
        module=self.checker()
        good=('BoarOS: COMPOSITION owners held=0x100 released=0x100 reclaimed=0x2 '
              'oom=0x1 retirement=0x1 remote-retirement=0x1 waits=0x7b ledger=0x0 control=0x0\n')
        module.validate_controlled(good)
        for bad in ('',good.replace('released=0x100','released=0xff'),
                    good.replace('reclaimed=0x2','reclaimed=0x0'),
                    good.replace('oom=0x1','oom=0x0'),good.replace('retirement=0x1','retirement=0x0'),
                    good.replace('remote-retirement=0x1','remote-retirement=0x0'),
                    good.replace('control=0x0','control=0x1'),good+good):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_controlled(bad)
        single=good.replace('remote-retirement=0x1','remote-retirement=0x0')
        module.validate_controlled(single,cpus=1)
        sqlite=(single.replace('oom=0x1','oom=0x0').replace('retirement=0x1','retirement=0x0')
                .replace('waits=0x7b','waits=0x0'))
        for cpus in (1,2):
            module.validate_controlled(sqlite,sqlite=True,cpus=cpus)
            with self.assertRaises(RuntimeError):
                module.validate_controlled(sqlite.replace('reclaimed=0x2','reclaimed=0x0'),sqlite=True,cpus=cpus)
    def test_functional_stream_is_complete_and_exact(self):
        module=self.checker()
        lines='\n'.join(f'COMPOSITION CASE {case} OK' for case in module.CASES)+f'\nCOMPOSITION END {len(module.CASES)}\n'
        self.assertEqual(module.validate_functional(lines),list(module.CASES))
        for bad in (lines.replace(f'COMPOSITION END {len(module.CASES)}',f'COMPOSITION END {len(module.CASES)-1}'),
                    lines.replace('COMPOSITION CASE mm-reclaim OK',''),lines+lines):
            with self.assertRaises(RuntimeError):module.validate_functional(bad)

    def test_wait_scene_requires_claim_and_four_real_registrations(self):
        module=self.checker()
        lines='\n'.join(f'COMPOSITION CASE {case} OK' for case in module.WAIT_CASES)+'\nCOMPOSITION END 4\n'
        self.assertEqual(module.validate_functional(lines,wait=True),list(module.WAIT_CASES))
        good=('COMPOSITION wait claims=0x1 active=0x0\n'
              'COMPOSITION owners held=0x0 released=0x0 reclaimed=0x0 oom=0x0 '
              'retirement=0x0 remote-retirement=0x0 waits=0x4 ledger=0x0 control=0x0\n')
        module.validate_wait(good)
        for bad in ('',good+good,good.replace('claims=0x1','claims=0x0'),
                    good.replace('waits=0x4','waits=0x3'),good.replace('active=0x0','active=0x1')):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_wait(bad)

    def test_cpu_clock_scene_checks_real_borrows_and_release(self):
        module=self.checker()
        lines='\n'.join(f'COMPOSITION CASE {case} OK' for case in module.CLOCK_CASES)+'\nCOMPOSITION END 2\n'
        self.assertEqual(module.validate_functional(lines,clock=True),list(module.CLOCK_CASES))
        good=('COMPOSITION clock claims=0x2 active=0x0\n'
              'COMPOSITION owners held=0x0 released=0x0 reclaimed=0x0 oom=0x0 '
              'retirement=0x0 remote-retirement=0x0 waits=0x0 ledger=0x0 control=0x0\n')
        module.validate_clock(good)
        for bad in ('',good+good,good.replace('claims=0x2','claims=0x1'),
                    good.replace('active=0x0','active=0x1'),good.replace('control=0x0','control=0x1')):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_clock(bad)

    def test_remote_fault_race_requires_two_faults_one_stale_retry_and_two_successes(self):
        module=self.checker()
        good='COMPOSITION fault-race arrivals=0x2 stale=0x1 resolved=0x2\n'
        module.validate_fault_race(good+'COMPOSITION fault-race-syncs=0x1\n')
        for bad in ('',good.replace('stale=0x1','stale=0x0'),good+good,
                    good+'COMPOSITION fault-race-syncs=0x2\n',good+'COMPOSITION fault-race-syncs=0x0\n'):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_fault_race(bad)

    def test_file_fault_race_requires_resumed_retry_and_single_sync(self):
        module=self.checker()
        good=('COMPOSITION file-fault-race arrivals=0x2 retries=0x1 resumed=0x1 resolved=0x2\n'
              'COMPOSITION file-fault-race-syncs=0x1\n')
        module.validate_file_fault_race(good)
        for bad in ('',good.replace('retries=0x1','retries=0x0'),
                    good.replace('resumed=0x1','resumed=0x0'),
                    good.replace('file-fault-race-syncs=0x1','file-fault-race-syncs=0x2'),good+good):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_file_fault_race(bad)

    def test_ordinary_io_requires_issuing_cpu_copies_pins_batch_and_wait_owners(self):
        module=self.checker()
        good=('COMPOSITION ordinary-io phases=0xa local=0xa '
              'copy0=0x0 copy1=0x10082 batch=0x1 local-faults=0x1 writes=0x3 '
              'write-pin0=0x0 write-pin1=0x4 close=0x1 seek=0x1 fork=0x1\n')
        module.validate_ordinary_io(good)
        for bad in ('',good+good,good.replace('copy1=0x10082','copy1=0x10081'),
                    good.replace('copy0=0x0','copy0=0x1'),good.replace('batch=0x1','batch=0x0'),
                    good.replace('write-pin1=0x4','write-pin1=0x0'),good.replace('fork=0x1','fork=0x0'),
                    good.replace('local-faults=0x1','local-faults=0x0')):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_ordinary_io(bad)
        single=good.replace('copy0=0x0 copy1=0x10082','copy0=0x10082 copy1=0x0').replace(
            'write-pin0=0x0 write-pin1=0x4','write-pin0=0x4 write-pin1=0x0')
        module.validate_ordinary_io(single,cpus=1)
        with self.assertRaises(RuntimeError):module.validate_ordinary_io(good,cpus=1)

    def test_build_root_can_isolate_toolchain_specific_objects(self):
        module=self.checker()
        with tempfile.TemporaryDirectory() as directory:
            requested=Path(directory)/'separate-build'
            self.assertEqual(module.resolve_build_root(requested),requested.resolve())
        self.assertEqual(module.resolve_build_root(None),module.ROOT/'build/riscv-user-smp-composition')

if __name__=='__main__':unittest.main()
