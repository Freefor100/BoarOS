#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/bpf
for shift in ${BPF_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/bpf_filesystems.c tests/host/mm/context.c kernel/cpu.c kernel/raw_lock.c kernel/credentials.c kernel/posix_acl.c fs/vfs.c \
        tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c mm/heap.c mm/memory_object.c \
        fs/bpffs.c fs/tracefs.c fs/sysfs.c \
        -Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed,--wrap=physical_page_allocate,--wrap=kernel_vfs_node_find,--wrap=kernel_vfs_node_lock,--wrap=kernel_vfs_publish_node,--wrap=kernel_vfs_nodes_idle \
        ${BPF_HOST_FLAGS:-} -o "build/host/bpf/filesystems-$shift"
    timeout 30s "build/host/bpf/filesystems-$shift"
done
