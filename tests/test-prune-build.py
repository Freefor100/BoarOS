#!/usr/bin/env python3
"""Keep reusable architecture inputs while selecting ephemeral native outputs."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC=importlib.util.spec_from_file_location('prune_build',Path(__file__).with_name('prune-build.py'))
prune=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(prune)

class CleanupContract(unittest.TestCase):
    def test_reusable_inputs_and_runtime_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);build=root/'build'
            caches=('qemu-la/qemu-system-loongarch64','qemu-la-rtc/source/rtc.c',
                'qemu-la-rtc/build/build.ninja','linux-la-platform/vmlinux',
                'loongarch/program-libc/source/entry.c','loongarch/dynamic-dp-v2/root/lib/libc.so',
                'loongarch/glibc/static-program','loongarch/diff-abi-cases')
            outputs=('loongarch/inventory-full/state.json','loongarch/program-inventory/state.json',
                'loongarch/review-boundary-green/linux.log','loongarch/diff-abi-run/linux.img',
                'loongarch/rtc-model.case/state.bin','loongarch/sqlite-run.case/root.img',
                'loongarch/program-libc/upstream.tar','loongarch/program-libc/build.log',
                'riscv/environment-run.case/root.img','riscv/glibc/run.case/fixture.img',
                'loongarch/glibc/static.build.log')
            for name in caches+outputs:
                path=build/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture')
            saved=(prune.ROOT,prune.BUILD,prune.current_linux_keys)
            try:
                prune.ROOT=root;prune.BUILD=build;prune.current_linux_keys=lambda:set()
                candidates=prune.candidates()
                selected=lambda path:any(path==item or path.is_relative_to(item) for item in candidates)
                for name in caches:self.assertFalse(selected(build/name),name)
                for name in outputs:self.assertTrue(selected(build/name),name)
            finally:prune.ROOT,prune.BUILD,prune.current_linux_keys=saved

if __name__=='__main__':unittest.main()
