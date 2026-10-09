#!/usr/bin/env python3
"""Pinned runtime identity includes permissions and link targets, not mtime."""
import importlib.util
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('profile',ROOT/'tests/userland/glibc/profiles.py')
profile=importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
sys.path.insert(0,str(ROOT/'tests/userland/glibc'))
run_spec=importlib.util.spec_from_file_location('glibc_runner',ROOT/'tests/userland/glibc/run.py')
runner=importlib.util.module_from_spec(run_spec)
run_spec.loader.exec_module(runner)

class Installation(unittest.TestCase):
    def test_each_identity_change(self):
        for change in ('content','mode','link','missing'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);(root/'header').write_text('original');(root/'link').symlink_to('header')
                expected=profile.tree_digest(root)
                self.assertEqual(profile.tree_digest(root),expected)
                if change=='content':(root/'header').write_text('modified')
                elif change=='mode':(root/'header').chmod((root/'header').stat().st_mode ^ 0o100)
                elif change=='link':(root/'link').unlink();(root/'link').symlink_to('other')
                else:(root/'header').unlink()
                self.assertNotEqual(profile.tree_digest(root),expected)

class Execution(unittest.TestCase):
    def test_rv_both_memory_sizes_use_same_program_and_both_kernels(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); program=root/'probe'; program.write_bytes(b'ELF fixture')
            def fixture(case, elf, library, inputs):
                self.assertEqual(elf,program)
                disk=case/'base.img';disk.write_bytes(elf.read_bytes());return disk
            commands=[]
            with patch.object(runner,'BUILD',root),patch.object(runner,'fixture',side_effect=fixture), \
                 patch.object(runner,'run_guest',side_effect=lambda argv,log:commands.append(argv) or 0), \
                 patch.object(runner,'check_observation'),contextlib.redirect_stdout(io.StringIO()):
                runner.execute({'static':program},root/'dso',{'glibc_version':'fixture'},
                               root/'linux',root/'boaros','qemu')
            actual={(argv[argv.index('-m')+1],argv[argv.index('-kernel')+1]) for argv in commands}
            self.assertEqual(actual,{(ram,str(root/kernel)) for ram in ('512M','1G') for kernel in ('linux','boaros')})
            self.assertEqual(len(commands),4)

    def test_failure_in_second_memory_size_is_not_hidden(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);program=root/'probe';program.write_bytes(b'ELF fixture')
            def fixture(case,*unused):
                disk=case/'base.img';disk.write_bytes(b'disk');return disk
            def launch(argv,log):
                if argv[argv.index('-m')+1]=='1G':raise RuntimeError('1G failure')
                return 0
            with patch.object(runner,'BUILD',root),patch.object(runner,'fixture',side_effect=fixture), \
                 patch.object(runner,'run_guest',side_effect=launch),patch.object(runner,'check_observation'), \
                 contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError,'1G failure'):
                    runner.execute({'static':program},root/'dso',{'glibc_version':'fixture'},
                                   root/'linux',root/'boaros','qemu')
            self.assertTrue(list(root.glob('run.*')))

if __name__=='__main__':unittest.main()
