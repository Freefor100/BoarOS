#!/bin/sh
set -eu
mkdir -p build/host/mm
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
    -DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/mm_huge.c tests/host/mm/unavailable.c tests/host/mm/context.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c -Wl,--gc-sections -o build/host/mm/huge
timeout 30s build/host/mm/huge
