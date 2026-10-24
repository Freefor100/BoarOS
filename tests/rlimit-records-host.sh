#!/bin/sh
set -eu
output=${RLIMIT_HOST_OUTPUT:-build/host/rlimit-records}
mkdir -p "$(dirname "$output")"
for shift in 12 14; do
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT="$shift" -Itests/host/sync -idirafter include \
        -ffunction-sections -fdata-sections tests/host/rlimit_records.c \
        kernel/rlimit.c kernel/sched/process.c kernel/raw_lock.c \
        -Wl,--gc-sections -o "$output-$shift"
    timeout 15s "$output-$shift"
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT="$shift" -Itests/host/sync -idirafter include \
        -ffunction-sections -fdata-sections tests/host/nproc.c kernel/rlimit.c \
        kernel/credentials.c kernel/sched/process.c kernel/sched/lifetime.c kernel/raw_lock.c \
        -Wl,--gc-sections -o "$output-nproc-$shift"
    timeout 15s "$output-nproc-$shift"
done
