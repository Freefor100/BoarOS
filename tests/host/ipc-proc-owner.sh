#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/ipc-proc
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 \
        -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_PAGE_CACHE_READAHEAD_PAGES=0 -DIPC_PROC_OWNER_CASE \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include -fsanitize=address,undefined \
        tests/host/ipc_proc_owner.c tests/host/pidfd_close_unavailable.c kernel/namespace.c \
        kernel/pid.c kernel/sched/proc.c kernel/sched/identity.c kernel/raw_lock.c \
        -Wl,--gc-sections -o "build/host/ipc-proc/owner-$shift"
    ASAN_OPTIONS=detect_leaks=0 "build/host/ipc-proc/owner-$shift"
done
