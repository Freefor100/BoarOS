#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/pid-namespace-futex
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -fsanitize=address,undefined -DBOAROS_MAX_CPUS=2 -DBOAROS_LOCK_DEBUG=1 \
        -DBOAROS_PAGE_SHIFT="$shift" -Itests/host/sync -idirafter include \
        tests/host/pid_namespace_futex.c kernel/pid.c kernel/raw_lock.c \
        -Wl,--gc-sections -o "build/host/pid-namespace-futex/view-$shift"
    ASAN_OPTIONS=detect_leaks=0 timeout 15s "build/host/pid-namespace-futex/view-$shift"
done
