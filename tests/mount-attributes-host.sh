#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${MOUNT_ATTRIBUTES_HOST_BUILD:-build/host/mount-attributes}
mkdir -p "$build_dir"
for shift in 12 14; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -DMM_TEST_EPOLL -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" -ffunction-sections -fdata-sections \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/mount_attributes.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
        kernel/syscall/memory.c fs/files/table.c fs/files/epoll.c fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c \
        kernel/credentials.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
        -Wl,--gc-sections -o "$build_dir/check-$shift"
    for group in execute devices mapping admission locking remount remount-race remount-sync; do
        timeout 30s "$build_dir/check-$shift" "$group"
    done
done
