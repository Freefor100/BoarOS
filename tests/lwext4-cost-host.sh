#!/bin/sh
# Measure public timestamp/data paths with independent block-device counters.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
    -include "$root/tests/host/lwext4_memory.h" \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_metadata.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
for replica in 0 1 2; do
    truncate -s 32M "$work/disk.img"
    mkfs.ext4 -q -F -b 4096 -I 256 "$work/disk.img"
    "$work/probe" "$work/disk.img" seed
    "$work/probe" "$work/disk.img" cost
    e2fsck -fn "$work/disk.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
done
