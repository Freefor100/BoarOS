#!/bin/sh
set -eu
output=${MM_LIMITS_HOST_OUTPUT:-build/host/mm/resource-limits}
mkdir -p "$(dirname "$output")"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 \
    -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 \
    -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/mm_resource_limits.c tests/host/mm/unavailable.c tests/host/mm/context.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    -Wl,--gc-sections,--wrap=kernel_heap_resize,--wrap=physical_page_allocate,--wrap=kernel_rwlock_write -o "$output"
timeout 15s "$output"
# VMA accounting uses real allocator/heap at both page sizes; no RV page table is
# presented as a 16KiB backend. Actual 16KiB MM faults run in the LA guest.
for shift in 12 14; do
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/vma_resource_limits.c tests/host/mm/unavailable.c tests/host/mm/context.c \
        kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/vma.c -Wl,--gc-sections -o "$output-vma-$shift"
    timeout 15s "$output-vma-$shift"
done
