#!/bin/sh
set -eu
mkdir -p build/host/hugetlb
for shift in 12 14; do
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=$shift \
       -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
       tests/host/hugetlb.c tests/host/mm/context.c tests/host/mm/unavailable.c \
       kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
       kernel/physical_page.c mm/heap.c mm/hugetlb.c -Wl,--gc-sections -o build/host/hugetlb/test-$shift
    timeout 40s build/host/hugetlb/test-$shift
done
