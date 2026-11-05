#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build_dir=${FLOCK_HOST_BUILD:-build/host/flock}
mkdir -p "$build_dir"
${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
    -DBOAROS_PAGE_SHIFT=12 -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 \
    -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/flock.c fs/flock.c kernel/cpu.c kernel/raw_lock.c \
    tests/host/mm/context.c tests/host/sync/notifications.c kernel/sched/park.c \
    -Wl,--gc-sections,--wrap=kernel_wait_park -o "$build_dir/cases"
timeout 20s "$build_dir/cases"
