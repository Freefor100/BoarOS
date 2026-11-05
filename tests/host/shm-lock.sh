#!/bin/sh
set -eu
output=${SHM_LOCK_HOST_OUTPUT:-build/host/shm-lock}
mkdir -p "$output"
for shift in ${SHM_LOCK_HOST_SHIFTS:-12 14}; do
    cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/shm_lock.c tests/host/mm/context.c kernel/credentials.c mm/shm.c \
        kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c \
        -Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_memory_object_create -o "$output/shm-lock-$shift"
    timeout 15s "$output/shm-lock-$shift"
done
