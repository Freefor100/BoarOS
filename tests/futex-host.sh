#!/bin/sh
set -eu
mkdir -p build/host/futex
for shift in 12 14; do
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=4 -ffunction-sections -fdata-sections \
    -DBOAROS_PAGE_SHIFT="$shift" -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/futex.c tests/host/mm/unavailable.c tests/host/mm/context.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/sched/futex.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    -Wl,--gc-sections,--wrap=kernel_rwlock_write,--wrap=kernel_wait_prepare,--wrap=kernel_memory_object_prepare_page,--wrap=kernel_raw_lock_acquire,--wrap=kernel_wait_backend_notify -o "build/host/futex/direct-$shift"
timeout 30s "build/host/futex/direct-$shift"
done
