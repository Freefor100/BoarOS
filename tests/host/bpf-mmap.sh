#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/bpf
${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
    -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 -DBOAROS_PAGE_SHIFT=12 -DMM_TEST_BPF \
    -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/bpf_mmap.c tests/host/bpf_mmap_unavailable.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
    fs/vfs.c fs/tmpfs.c fs/open_file.c fs/block_file.c kernel/block.c fs/file_mapping.c fs/record_lock.c fs/flock.c \
    kernel/cpu.c kernel/raw_lock.c kernel/credentials.c kernel/posix_acl.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    kernel/bpf/objects.c kernel/bpf/maps.c kernel/bpf/ringbuf.c kernel/bpf/runtime.c kernel/bpf/trace.c kernel/bpf/verify.c kernel/bpf/vm.c \
    ${BPF_HOST_FLAGS:-} -Wl,--gc-sections,--wrap=physical_page_allocate -o build/host/bpf/mmap-12
timeout 30s build/host/bpf/mmap-12
