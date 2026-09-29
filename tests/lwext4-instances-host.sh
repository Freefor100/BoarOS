#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_instances.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
for size in 1024 4096; do
    truncate -s 32M "$work/a.img" "$work/b.img"
    mkfs.ext4 -q -F -b "$size" "$work/a.img"
    mkfs.ext4 -q -F -b "$size" "$work/b.img"
    timeout 30 "$work/probe" "$work/a.img" "$work/b.img"
    e2fsck -fn "$work/a.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    e2fsck -fn "$work/b.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    printf 'PASS: independent ext4 instances, %s-byte blocks\n' "$size"
done
