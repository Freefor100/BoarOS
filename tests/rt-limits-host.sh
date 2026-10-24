#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/host/rt-limits
for shift in 12 14; do
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=$shift -ffunction-sections -fdata-sections \
      -Itests/host/sync -idirafter include tests/host/rt_limits.c kernel/sched/scheduling.c \
      kernel/sched/core.c kernel/sched/policy.c kernel/sched/runqueue.c kernel/sched/park.c \
      kernel/cpu.c kernel/raw_lock.c kernel/credentials.c tests/host/sync/notifications.c \
      -Wl,--gc-sections -o build/host/rt-limits/$shift
    build/host/rt-limits/$shift
done
