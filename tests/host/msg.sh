#!/bin/sh
# Real queue/raw/wait implementation with controlled machine and heap boundaries.
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build/as-data-msg-20261024/msg-host}
mkdir -p "$out"
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
        -DBOAROS_MAX_CPUS=4 -DBOAROS_LOCK_DEBUG=1 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        -fsanitize=address,undefined tests/host/msg.c kernel/raw_lock.c kernel/sched/park.c \
        -Wl,--gc-sections,--wrap=kernel_wait_park,--wrap=kernel_wait_notify,--wrap=kernel_raw_lock_acquire \
        -o "$out/cases-$shift"
    timeout -k 2s 30s "$out/cases-$shift"
done
