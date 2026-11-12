#!/usr/bin/env python3
"""Keep reusable architecture inputs while selecting ephemeral native outputs."""
import sys
from pathlib import Path
sys.path[:0] = [str(Path(__file__).resolve().parent), str(Path(__file__).resolve().parents[1])]
import importlib.util
import tempfile
import unittest

SPEC=importlib.util.spec_from_file_location('prune_build',Path(__file__).with_name('prune-build.py'))
prune=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(prune)

class CleanupContract(unittest.TestCase):
    def test_reusable_inputs_and_runtime_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);build=root/'build'
            caches=('qemu-la-rtc/qemu-system-loongarch64',
                'tools/loongarch/bin/gcc', 'loongarch/musl-root/bin/musl-gcc',
                'riscv-user-smp4/riscv/musl-root/bin/musl-gcc',
                'local-keep/source.tar', 'loongarch/gcc-sf/root/bin/gcc',
                'tools/oscomp-local/riscv/mke2fs')
            outputs=('host/cache/concurrency-12', 'io-budget/kernels/profile/kernel-rv',
                'riscv/kernel/main.o', 'loongarch/fs/vfs.o',
                'riscv-user-smp8/riscv/mm/mm.o', 'riscv-user-smp8/kernel-rv',
                'loongarch-smp8/kernel-la', 'loongarch/glibc/run.case/root.img',
                'tools/oscomp-local/riscv/scratch-template.img',
                'bpf/riscv/root/usr/bin/bpftrace',
                'bpf/loongarch/llvm/lib/libLLVM.so.23.1',
                'bpf/riscv/linux/arch/riscv/boot/Image',
                'linux-la-platform/vmlinux', 'offline-c/alpine-tree/usr/bin/gcc',
                'diff-abi/linux/current/arch/riscv/boot/Image')
            for name in caches+outputs:
                path=build/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('fixture')
            saved=(prune.ROOT,prune.BUILD)
            try:
                prune.ROOT=root;prune.BUILD=build
                candidates=prune.candidates()
                selected=lambda path:any(path==item or path.is_relative_to(item) for item in candidates)
                for name in caches:self.assertFalse(selected(build/name),name)
                for name in outputs:self.assertTrue(selected(build/name),name)
            finally:prune.ROOT,prune.BUILD=saved

    def test_cleanup_does_not_remove_a_worktree_or_follow_parent_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); build=root/'build'
            worktree=build/'scratch'/'checkout'; worktree.mkdir(parents=True)
            external=root/'external'; external.mkdir()
            (build/'linked').symlink_to(external,target_is_directory=True)
            saved=prune.BUILD
            try:
                prune.BUILD=build
                paths=[worktree.parent,worktree,worktree/'notes',build/'ordinary-run',
                       build/'linked',build/'linked'/'outside.img']
                self.assertEqual(prune.deletion_paths(paths,[worktree]),
                                 [build/'ordinary-run',build/'linked'])
            finally:prune.BUILD=saved

if __name__=='__main__':unittest.main()
