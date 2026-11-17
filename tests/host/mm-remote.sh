#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build_dir=${MM_REMOTE_HOST_BUILD:-build/host/mm}
mkdir -p "$build_dir"
for shift in 12 14; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -pthread \
        -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" -ffunction-sections -fdata-sections \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/mm_remote.c tests/host/mm/unavailable.c tests/host/mm/context.c \
        kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/hugetlb.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
        -Wl,--gc-sections -o "$build_dir/remote-$shift"
    timeout 30s "$build_dir/remote-$shift"
done
