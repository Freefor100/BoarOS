#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/bpf
for shift in ${BPF_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 ${BPF_HOST_OPT:--O2} -g -Wall -Wextra -Werror -pthread \
        -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/bpf_maps.c tests/host/mm/context.c tests/host/sync/notifications.c \
        kernel/cpu.c kernel/raw_lock.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c kernel/bpf/maps.c kernel/bpf/ringbuf.c \
        ${BPF_HOST_FLAGS:-} -Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed,--wrap=physical_page_allocate -o "build/host/bpf/maps-$shift"
    timeout 30s "build/host/bpf/maps-$shift"
done
