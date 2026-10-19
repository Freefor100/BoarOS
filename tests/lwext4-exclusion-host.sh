#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for cpus in 1 2; do
    for debug in 0 1; do
        ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
            -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT=12 \
            -DCONFIG_USE_DEFAULT_CFG=1 -DBOAROS_MAX_CPUS="$cpus" -DBOAROS_LOCK_DEBUG="$debug" \
            -I"$root/third_party/lwext4/include" -I"$root/tests/host/sync" -idirafter "$root/include" \
            "$root/tests/host/lwext4_exclusion.c" "$root/kernel/cpu.c" \
            "$root/kernel/raw_lock.c" "$root/tests/host/sync/notifications.c" \
            -Wl,--gc-sections,--wrap=kernel_raw_lock_acquire -o "$work/probe"
        "$work/probe"
    done
done
