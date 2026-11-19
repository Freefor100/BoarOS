#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host
cc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT=12 -ffunction-sections -fdata-sections \
    -Itests/host/sync -idirafter include tests/host/mount_pid_view.c -Wl,--gc-sections -o build/host/mount-pid-view
build/host/mount-pid-view
