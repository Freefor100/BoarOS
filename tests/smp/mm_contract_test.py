#!/usr/bin/env python3
"""Expected faults require new-path owner evidence and cannot claim closure."""
import importlib.util
from pathlib import Path
import unittest
import subprocess
spec = importlib.util.spec_from_file_location('smp_runner', Path(__file__).with_name('riscv.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class MMReport(unittest.TestCase):
    def test_failure_requires_owner_and_exact_mm_stage(self):
        for profile, stage, owner in (
            ('rfence-error', 'MM-RFENCE', 'MM RFENCE error retains data/table batch owners'),
            ('mm-fence-i', 'MM-FENCE.I', 'MM FENCE.I error retains live MM/code owners')):
            text = 'BoarOS: SMP failure stage=' + stage + '\n' + owner
            runner.validate_mm_failure(text, profile)
            for invalid in (owner, 'SMP failure stage=' + stage,
                            text + '\nMM SMP natural exit', text.replace('stage=MM-', 'stage=')):
                with self.subTest(profile=profile, invalid=invalid):
                    with self.assertRaises(RuntimeError): runner.validate_mm_failure(invalid, profile)
    def test_scheduler_failure_is_not_mm_evidence(self):
        with self.assertRaises(ValueError): runner.validate_mm_failure('SMP failure stage=HSM-start', 'denied-start')
    def test_shared_fault_requires_shared_owners(self):
        text='SMP failure stage=MM-RFENCE\nshared MM RFENCE error retains object/pin/data/table owners'
        runner.validate_mm_failure(text,'rfence-error','mm-shared')
        for invalid in (text.replace('shared MM','MM'), text+'\nnatural exit',
                        'SMP failure stage=MM-RFENCE\nMM RFENCE error retains data/table batch owners'):
            with self.assertRaises(RuntimeError): runner.validate_mm_failure(invalid,'rfence-error','mm-shared')
    def test_invalid_mm_build_profiles_fail_before_build(self):
        for arguments, expected in (
            (['MM_SMP_TEST=1'], 'MM_SMP_TEST requires'),
            (['MM_SHARED_SMP_TEST=1'], 'MM_SHARED_SMP_TEST requires'),
            (['SMP=1', 'SMP_TEST=1', 'MM_SMP_TEST=1', 'MM_SHARED_SMP_TEST=2'], 'MM_SHARED_SMP_TEST must be'),
            (['SMP=1', 'SMP_TEST=1', 'MM_SMP_TEST=1', 'MM_SHARED_SMP_TEST=1', 'SMP_TEST_PROFILE=mm-fence-i'], 'MM_SHARED_SMP_TEST profile'),
            (['SMP=1', 'SMP_TEST=1', 'MM_SMP_TEST=1', 'SMP_TEST_PROFILE=guard'], 'MM_SMP_TEST profile'),
            (['SMP=1', 'SMP_TEST=1', 'MM_SMP_TEST=1', 'COST_DIAGNOSTICS=1'], 'SMP plus COST')):
            result = subprocess.run(['make', '-n', *arguments, 'all'], cwd=Path(__file__).resolve().parents[2],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected, result.stderr)

if __name__ == '__main__': unittest.main()
