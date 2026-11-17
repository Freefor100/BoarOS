#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/cgroup
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include -fsanitize=address,undefined \
        tests/host/cgroup_pid_view.c kernel/sched/identity.c kernel/pid.c kernel/cpu.c kernel/raw_lock.c \
        -Wl,--gc-sections -o "build/host/cgroup/pid-view-$shift"
    ASAN_OPTIONS=detect_leaks=0 "build/host/cgroup/pid-view-$shift"
done
