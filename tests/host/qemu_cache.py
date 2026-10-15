"""A verified frozen emulator is reusable without relinking host dependencies."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/loongarch'))
spec=importlib.util.spec_from_file_location('qemu_rtc_cache',ROOT/'tests/loongarch/qemu_rtc.py')
cache=importlib.util.module_from_spec(spec);spec.loader.exec_module(cache)

class FrozenEmulator(unittest.TestCase):
    def check(self,change=None):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'upstream';source.mkdir();derived=root/'cache/source';derived.mkdir(parents=True)
            subprocess.run(['git','init','-q',str(source)],check=True)
            (source/'input').write_text('fixed source');subprocess.run(['git','-C',str(source),'add','input'],check=True)
            (derived/'input').write_text('fixed source')
            binary=root/'cache/qemu-system-loongarch64';binary.write_text('identified native fixture');binary.chmod(0o755)
            record={'revision':'fixed','patch':cache.digest(cache.PATCH),'compiler':'compiler',
                'configuration':cache.QEMU_OPTIONS,'profile':'ls7a-rtc-pm-direct-irq-v1',
                'source_files':cache.source_files(source,derived),'dependencies':{},
                'binary':cache.product(binary),'configuration_files':{}}
            (root/'cache/boaros-identity.json').write_text(json.dumps(record))
            if change=='binary':binary.write_text('changed')
            if change=='mode':binary.chmod(0o644)
            if change=='source':(derived/'input').write_text('changed')
            real_run=subprocess.run
            def read_only(command,*args,**kwargs):
                if command[0]!='git':raise AssertionError('a frozen emulator must not run ninja')
                return real_run(command,*args,**kwargs)
            with patch.object(cache,'DIRECTORY',root/'cache'),patch.object(cache,'fixed_repository',return_value=(source,'fixed')), \
                 patch.object(cache,'compiler_identity',return_value='compiler'),patch.object(cache,'require_user_network') as capability, \
                 patch.object(cache.subprocess,'run',side_effect=read_only),contextlib.redirect_stdout(io.StringIO()):
                if change:
                    with self.assertRaises(RuntimeError):cache.prepare(2)
                    capability.assert_not_called()
                else:
                    cache.prepare(2);capability.assert_called_once_with(binary)
    def test_valid_binary_does_not_rebuild_obsolete_host_paths(self):self.check()
    def test_identity_changes_are_not_a_cache_hit(self):
        for change in ('binary','mode','source'):
            with self.subTest(change=change):self.check(change)

if __name__=='__main__':unittest.main()
