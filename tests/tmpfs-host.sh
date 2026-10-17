#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${TMPFS_HOST_BUILD:-build/host/tmpfs}
mkdir -p "$build_dir"
for shift in ${TMPFS_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/tmpfs_test.c tests/host/mm/context.c kernel/cpu.c kernel/raw_lock.c \
        tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c \
        -Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed \
        ${TMPFS_HOST_FLAGS:-} -o "$build_dir/tmpfs-$shift"
    timeout 30s "$build_dir/tmpfs-$shift"
done
