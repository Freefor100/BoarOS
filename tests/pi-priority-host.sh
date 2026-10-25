#!/bin/sh
set -eu
mkdir -p build/host/pi-priority
for shift in 12 14; do
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=4 -DBOAROS_USER_SMP=1 -DBOAROS_PAGE_SHIFT="$shift" -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include tests/host/pi_priority.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/park.c kernel/sched/scheduling.c kernel/sched/futex_pi.c kernel/sched/policy.c kernel/sched/runqueue.c -Wl,--gc-sections -o build/host/pi-priority/direct-$shift
timeout 15s build/host/pi-priority/direct-$shift
done
