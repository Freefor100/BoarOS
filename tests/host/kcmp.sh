#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build/host/kcmp}
mkdir -p "$out"
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_USER_SMP=1 -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_LOCK_DEBUG=1 \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        -fsanitize=address,undefined tests/host/kcmp.c kernel/raw_lock.c \
        -Wl,--gc-sections,--wrap=kernel_raw_lock_acquire -o "$out/query-$shift"
    timeout -k 1s 10s "$out/query-$shift"
done
