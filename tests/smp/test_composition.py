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
    def test_functional_stream_is_complete_and_exact(self):
        module=self.checker()
        lines='\n'.join(f'COMPOSITION CASE {case} OK' for case in module.CASES)+f'\nCOMPOSITION END {len(module.CASES)}\n'
        self.assertEqual(module.validate_functional(lines),list(module.CASES))
        for bad in (lines.replace(f'COMPOSITION END {len(module.CASES)}',f'COMPOSITION END {len(module.CASES)-1}'),
                    lines.replace('COMPOSITION CASE mm-reclaim OK',''),lines+lines):
            with self.assertRaises(RuntimeError):module.validate_functional(bad)

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

    def test_ordinary_reads_require_real_hart_copies_continuation_and_wait_owners(self):
        module=self.checker()
        good=('COMPOSITION ordinary-read phases=0x9 local=0x5 service=0x4 '
              'copy0=0x2e copy1=0x54 migrations=0x5 borrowed=0x1 close=0x1 seek=0x1 fork=0x1\n')
        module.validate_ordinary_reads(good)
        for bad in ('',good+good,good.replace('copy1=0x54','copy1=0x0'),
                    good.replace('copy0=0x2e','copy0=0x2f'),good.replace('migrations=0x5','migrations=0x0'),
                    good.replace('borrowed=0x1','borrowed=0x0'),good.replace('fork=0x1','fork=0x0')):
            with self.subTest(bad=bad),self.assertRaises(RuntimeError):module.validate_ordinary_reads(bad)
        single=good.replace('copy0=0x2e copy1=0x54 migrations=0x5 borrowed=0x1',
                            'copy0=0x82 copy1=0x0 migrations=0x0 borrowed=0x0')
        module.validate_ordinary_reads(single,cpus=1)
        with self.assertRaises(RuntimeError):module.validate_ordinary_reads(good,cpus=1)

    def test_build_root_can_isolate_toolchain_specific_objects(self):
        module=self.checker()
        with tempfile.TemporaryDirectory() as directory:
            requested=Path(directory)/'separate-build'
            self.assertEqual(module.resolve_build_root(requested),requested.resolve())
        self.assertEqual(module.resolve_build_root(None),module.ROOT/'build/riscv-user-smp-composition')

if __name__=='__main__':unittest.main()
