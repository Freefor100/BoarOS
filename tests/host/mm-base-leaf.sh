#!/bin/sh
set -eu
mkdir -p build/host/mm
for shift in 12 14; do
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
    -DBOAROS_PAGE_SHIFT="$shift" -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/mm_base_leaf.c tests/host/mm/unavailable.c tests/host/mm/context.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/hugetlb.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c -Wl,--gc-sections -o build/host/mm/base-leaf-$shift
timeout 30s build/host/mm/base-leaf-$shift
done
