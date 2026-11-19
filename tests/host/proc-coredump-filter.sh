#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/coredump-filter
# Production proc routing/parser with modeled target-MM provider.
for shift in 12 14; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT=$shift -ffunction-sections -fdata-sections \
        ${PROC_FILTER_TEST_CFLAGS:-} -Itests/host/sync -idirafter include \
        tests/host/proc_coredump_filter.c -Wl,--gc-sections -o build/host/coredump-filter/proc-$shift
    build/host/coredump-filter/proc-$shift
    ${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT=$shift -ffunction-sections -fdata-sections \
        ${PROC_FILTER_TEST_CFLAGS:-} -Itests/host/sync -idirafter include \
        tests/host/core_pid_view.c kernel/sched/identity.c kernel/pid.c kernel/cpu.c kernel/raw_lock.c \
        -Wl,--gc-sections -o build/host/coredump-filter/core-view-$shift
    build/host/coredump-filter/core-view-$shift
done
