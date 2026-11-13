#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build_dir=${REBOOT_HOST_BUILD:-build/host/reboot}
mkdir -p "$build_dir"
for shift in 12 14; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=4 -DBOAROS_LOCK_DEBUG=1 \
        -Itests/host/sync -idirafter include \
        tests/host/reboot.c kernel/reboot.c kernel/raw_lock.c -o "$build_dir/reboot-$shift"
    timeout 15s "$build_dir/reboot-$shift"
done
