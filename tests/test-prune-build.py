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
                'riscv-smp/riscv/kernel/main.o',
                'riscv-user-smp/riscv/mm/mm.o','riscv-user-smp/kernel-rv','riscv-user-smp/user-smp-rv','riscv-user-smp/user-smp-dynamic-rv', 'riscv-smp-test/guard/kernel-rv',
                'riscv-mm-smp-test/riscv/mm/mm.o', 'riscv-mm-smp-test/mm-fence-i/kernel-rv',
                'riscv-mm-shared-smp-test/riscv/mm/mm.o', 'riscv-mm-shared-smp-test/rfence-error/kernel-rv',
                'riscv-mm-file-smp-test/riscv/mm/mm.o', 'riscv-mm-file-smp-test/rfence-error/kernel-rv',
                'riscv-mm-disk-smp-test/riscv/mm/mm.o', 'riscv-mm-disk-smp-test/rfence-error/kernel-rv',
                'riscv-file-io-smp-test/riscv/fs/files/io.o',
                'riscv-file-io-smp-test/ci-unknown-elf/kernel-rv',
                'host/cache/concurrency-12','host/mm-disk/concurrency-12',
                'qemu-la-rtc/build/build.ninja','linux-la-platform/vmlinux',
                'loongarch/program-libc/source/entry.c','loongarch/dynamic-dp-v2/root/lib/libc.so',
                'loongarch/glibc/static-program','loongarch/diff-abi-cases')
            outputs=('loongarch/program-inventory-user-smp-review/state.json','loongarch/inventory-full/state.json','loongarch/program-inventory/state.json',
                'riscv-smp/runs/2-512M.log',
                'riscv-user-smp/runs/run-first/report.json','riscv-user-smp/abi/run-first/root.img',
                'riscv-user-smp/runtime/run-first/glibc/fixture.img',
                'riscv-mm-smp-test/runs/rfence-error/2-1G.log',
                'riscv-mm-shared-smp-test/runs/2-512M.log',
                'riscv-mm-file-smp-test/runs/2-512M.log',
                'riscv-mm-file-smp-test/runs-v3/1-1G.json',
                'riscv-mm-disk-smp-test/runs/2-512M/disk.img',
                'riscv-mm-disk-smp-test/runs-v2/1-1G.json',
                'riscv-file-io-smp-test/runs/2-512M/disk.img',
                'riscv-file-io-smp-test/ci-unknown-elf/runs/user-1G/root.img',
                'host/cache/image-14.img','host/mm-disk/run.log','host/cache/diagnostic.elf',
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
