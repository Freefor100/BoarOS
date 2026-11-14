#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
out=${MM_ACTIVITY_HOST_BUILD:-build/host/mm}
source=${MM_ACTIVITY_SOURCE:-mm/mm.c}
mkdir -p "$out"
for cpus in ${MM_ACTIVITY_CPUS:-2 4}; do
    cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS="$cpus" \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/mm_activity.c tests/host/mm/unavailable.c tests/host/mm/context.c \
        kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c "$source" mm/uaccess.c arch/riscv/sv39.c \
        -Wl,--gc-sections -o "$out/activity$cpus"
    timeout 30s "$out/activity$cpus"
done
