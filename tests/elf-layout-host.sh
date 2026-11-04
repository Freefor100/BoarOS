#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${ELF_LAYOUT_HOST_BUILD:-build/host/mm}
mkdir -p "$build_dir"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
    -DBOAROS_PAGE_SHIFT=12 -DMM_TEST_ELF_SOURCE=1 -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/elf_layout.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
    fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c fs/fs_context.c fs/file_mapping.c fs/record_lock.c fs/flock.c \
    kernel/elf64.c kernel/elf64_source.c kernel/elf_image.c kernel/read_source.c kernel/exec.c kernel/exec_image.c \
    kernel/credentials.c kernel/posix_acl.c kernel/block.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    -Wl,--gc-sections,--wrap=kernel_vfs_close -o "$build_dir/elf-layout"
timeout 30s "$build_dir/elf-layout"
