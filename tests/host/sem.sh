#!/bin/sh
# The guest workload checks the syscall ABI; this fixture controls allocation
# failures and the RMID/last-owner interleaving with real raw and wait code.
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build/host/sem}
mkdir -p "$out"
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
        -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        -fsanitize=address,undefined tests/host/sem.c kernel/raw_lock.c kernel/sched/park.c \
        -Wl,--gc-sections,--wrap=kernel_raw_lock_acquire -o "$out/cases-$shift"
    timeout -k 2s 30s "$out/cases-$shift"
done
