#!/usr/bin/env python3
"""Check BPF source identity rejection and cross-build boundaries."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bpf_build', ROOT/'tests/workloads/bpf/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

class Inputs(unittest.TestCase):
    def test_wrong_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'source.tar.gz'
            path.write_bytes(b'bad archive')
            with self.assertRaisesRegex(RuntimeError, 'SHA-256'):
                build.verify_archive(path, '0'*64)

    def test_manifest_has_unique_source_identity(self):
        rows = build.references()
        self.assertEqual(rows['linux'][4], build.LINUX_REVISION)
        for source in build.ARCHIVES.values():
            self.assertEqual(rows[source][0], 'file')
            self.assertRegex(rows[source][4], r'^[0-9a-f]{64}$')

    def test_target_architecture_is_explicit(self):
        for arch, machine in [('riscv', 'riscv64'), ('loongarch', 'loongarch64')]:
            values = build.llvm_options(arch)
            self.assertIn('-DCMAKE_SYSTEM_NAME=Linux', values)
            self.assertIn('-DCMAKE_SYSTEM_PROCESSOR='+machine, values)
            self.assertIn('-DLLVM_TARGETS_TO_BUILD=BPF', values)
            self.assertIn('-DLLVM_TABLEGEN=/usr/bin/llvm-tblgen', values)
            self.assertFalse(any('/usr/include' in value for value in values))

    def test_release_archive_cannot_inherit_parent_git_identity(self):
        build.BASE.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build.BASE) as directory:
            result = subprocess.run(['git','-C',directory,'rev-parse','--show-toplevel'],
                env=build.environment('riscv'),capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)

if __name__ == '__main__': unittest.main()
