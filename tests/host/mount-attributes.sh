#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build_dir=${MOUNT_ATTRIBUTES_HOST_BUILD:-build/host/mount-attributes}
mkdir -p "$build_dir"
for shift in ${MOUNT_ATTRIBUTES_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -DMM_TEST_EPOLL -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" -ffunction-sections -fdata-sections \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/mount_attributes.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
        kernel/syscall/memory.c fs/files/table.c fs/files/epoll.c fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c fs/flock.c \
        kernel/credentials.c kernel/posix_acl.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
        -Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_heap_allocate,--wrap=kernel_vfs_mount_initialize -o "$build_dir/check-$shift"
    for group in ${MOUNT_ATTRIBUTES_HOST_GROUPS:-move cwd-unreachable executable-path execute devices mapping admission locking remount remount-race remount-sync views view-tree view-cleanup view-sync view-sync-publish view-deep view-chain view-sync-owner}; do
        timeout 30s "$build_dir/check-$shift" "$group"
    done
done
