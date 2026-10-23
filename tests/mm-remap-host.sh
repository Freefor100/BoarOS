#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${MM_REMAP_HOST_BUILD:-build/host/mm}
mkdir -p "$build_dir"
${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
    -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=12 -ffunction-sections -fdata-sections \
    -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/mm_remap.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
    fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    -Wl,--gc-sections,--wrap=physical_page_allocate,--wrap=kernel_rwlock_write -o "$build_dir/remap"
timeout 30s "$build_dir/remap"
