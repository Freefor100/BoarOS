#!/usr/bin/env python3
"""Check BPF source identity rejection and cross-build boundaries."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bpf_build', ROOT/'tests/workloads/bpf/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)
spec = importlib.util.spec_from_file_location('host_pahole_build', ROOT/'tools/build-pahole.py')
pahole = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pahole)

class Inputs(unittest.TestCase):
    def test_offline_encoder_rejects_corrupt_bundled_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            (root/'references').mkdir();(root/'third_party/sources').mkdir(parents=True)
            (root/'references/sources.tsv').write_text((ROOT/'references/sources.tsv').read_text())
            for name in pahole.ARCHIVES:
                (root/'third_party/sources'/name).write_bytes(b'corrupt source archive')
            with patch.object(pahole,'ROOT',root):
                with self.assertRaisesRegex(RuntimeError,'SHA-256'):
                    pahole.sources()

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
