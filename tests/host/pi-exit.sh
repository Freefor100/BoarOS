#!/bin/sh
set -eu
mkdir -p build/host/pi-exit
for shift in 12 14; do
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=4 -DBOAROS_PAGE_SHIFT="$shift" -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include tests/host/pi_exit.c tests/host/mm/context.c kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/sched/futex_pi.c -Wl,--gc-sections -o build/host/pi-exit/direct-$shift
timeout 15s build/host/pi-exit/direct-$shift
done
