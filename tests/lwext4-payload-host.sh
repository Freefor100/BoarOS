#!/bin/sh
# Borrow old cache payloads across publication and completion without a journal.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DBOAROS_MAX_CPUS=2 -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
    -include "$root/tests/host/lwext4_memory.h" \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root/third_party/lwext4/src/ext4_bcache.c" \
    "$root/third_party/lwext4/src/ext4_blockdev.c" \
    "$root/tests/host/lwext4_payload.c" "$root/tests/host/block_runtime.c" \
    "$root/kernel/raw_lock.c" "$root/tests/host/sync/notifications.c" \
    -I"$root/tests/host/block_model" -pthread -o "$work/probe"
"$work/probe" 1024
"$work/probe" 4096
