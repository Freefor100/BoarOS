"""Project input identity must describe the files actually booted."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import sys

SPEC=importlib.util.spec_from_file_location('offline_c_runner',Path(__file__).with_name('offline-c-riscv.py'))
runner=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(runner)

class SnapshotIdentity(unittest.TestCase):
    def test_rebuild_during_boot_does_not_change_identity(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);(root/'build/riscv').mkdir(parents=True)
            (root/'references/lua').mkdir(parents=True)
            (root/'references/lua/lua-5.4.3.tar.gz').write_bytes(b'fixed-release')
            tree=root/'toolchain';tree.mkdir();(tree/'compiler').write_bytes(b'old-compiler')
            kernel=root/'kernel';kernel.write_bytes(b'old-kernel')
            linux=root/'linux';linux.write_bytes(b'old-linux')
            program=root/'program';program.write_bytes(b'old-init')
            args=SimpleNamespace(kernel=kernel,program=program,linux_kernel=linux,toolchain_tree=tree,
                only=None,repeat=1,performance=True,jobserver_only=True,tmpfs=False,observe=False,
                qemu=sys.executable,timeout=1)
            original_toolchain=runner.tree_identity(tree)
            booted={}
            def fixture(directory,_,__):
                (directory/'tree').mkdir();(directory/'tree/init').write_bytes(program.read_bytes())
                image=directory/'fixture.img';image.write_bytes(b'fixed-disk');return image
            def boot(command,**kwargs):
                snapshot=Path(command[command.index('-kernel')+1]);booted[snapshot.name]=snapshot.read_bytes()
                # Normal rebuilds may replace the live inputs while old boot snapshots run.
                kernel.write_bytes(b'new-kernel-'+snapshot.name.encode())
                linux.write_bytes(b'new-linux');program.write_bytes(b'new-init');(tree/'compiler').write_bytes(b'new-compiler')
                kwargs['stdout'].write('PROJECT COMMAND probe cwd=/work/lua argv=true status=0 elapsed_ns=1\nPROJECT PASS all\nBoarOS: PID 1 exited status=0x2a heap-live=0x0; shutting down\n')
                return SimpleNamespace(returncode=0)
            def extract(_disk,_guest,destination):destination.write_text('probe\n');return True
            with patch.object(runner,'ROOT',root),patch.object(runner,'project_fixture',fixture),\
                 patch.object(runner,'linux_image',lambda _:linux),\
                 patch.object(runner,'copy_boot_disk',lambda source,dest:shutil.copyfile(source,dest)),\
                 patch.object(runner,'command',lambda *a:SimpleNamespace(stdout='fixed-source')),\
                 patch.object(runner.subprocess,'run',boot),\
                 patch.object(runner,'replay_and_check',lambda *a:None),\
                 patch.object(runner,'extract',extract),contextlib.redirect_stdout(io.StringIO()):
                runner.project_main(args)
            import json
            directory=next((root/'build/riscv').iterdir())
            identity=json.loads((directory/'identity.json').read_text())
            digest=lambda data:hashlib.sha256(data).hexdigest()
            self.assertEqual(identity['kernel'],digest(booted['boaros-kernel']))
            self.assertEqual(identity['driver'],digest(b'old-init'))
            self.assertEqual(identity['toolchain'],original_toolchain)
            self.assertEqual(identity['kernel_snapshots']['linux'],digest(booted['linux-kernel']))
            self.assertEqual(identity['kernel_snapshots']['boaros'],digest(booted['boaros-kernel']))

if __name__=='__main__':unittest.main()
