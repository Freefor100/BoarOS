#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
check_fs() {
    e2fsck -fn "$1" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
}
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
    -include "$root/tests/host/lwext4_memory.h" \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_metadata.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
for block in 1024 4096; do
    for inode in 128 256; do
        truncate -s 32M "$work/base.img"
        mkfs.ext4 -q -F -b "$block" -I "$inode" "$work/base.img"
        overhead=$(dumpe2fs -h "$work/base.img" 2>/dev/null | sed -n 's/^Overhead clusters:[[:space:]]*//p')
        "$work/probe" "$work/base.img" seed
        for mode in times readonly stats stats-ro unrelated read-stats; do
            cp "$work/base.img" "$work/test.img"
            "$work/probe" "$work/test.img" "$mode" "$overhead"
            check_fs "$work/test.img"
        done
        for mode in write-times flush-times; do
            cp "$work/base.img" "$work/test.img"
            "$work/probe" "$work/test.img" "$mode" "$overhead"
            "$work/probe" "$work/test.img" verify-times
            "$work/probe" "$work/test.img" verify-times
            check_fs "$work/test.img"
        done
        for damage in geometry free journal location checksum; do
            cp "$work/base.img" "$work/test.img"
            "$work/probe" "$work/test.img" corrupt "$damage"
        done
        for mode in oom-times oom-stats; do
            cp "$work/base.img" "$work/test.img"
            attempts=$("$work/probe" "$work/test.img" "$mode" "$overhead")
            point=1
            while [ "$point" -le "$attempts" ]; do
                cp "$work/base.img" "$work/test.img"
                "$work/probe" "$work/test.img" "$mode" "$overhead" "$point" > /dev/null
                check_fs "$work/test.img"
                point=$((point+1))
            done
            printf 'PASS: %s block=%s inode=%s allocations=%s\n' "$mode" "$block" "$inode" "$attempts"
        done
        printf 'PASS: metadata block=%s inode=%s\n' "$block" "$inode"
    done
done
truncate -s 8193K "$work/base.img"
mkfs.ext4 -q -F -b 1024 -O '^has_journal' "$work/base.img"
overhead=$(dumpe2fs -h "$work/base.img" 2>/dev/null | sed -n 's/^Overhead clusters:[[:space:]]*//p')
# mke2fs rounds this size down; adding one block fills group 0 exactly after
# first_data_block. It must not manufacture an empty second descriptor group.
resize2fs -f "$work/base.img" 8193 > "$work/resize.log" 2>&1
"$work/probe" "$work/base.img" seed
"$work/probe" "$work/base.img" stats "$overhead"
check_fs "$work/base.img"
printf 'PASS: geometry first_data_block at exact group boundary\n'
for features in 'meta_bg,^resize_inode' 'sparse_super2' '^metadata_csum,uninit_bg' '^has_journal'; do
    truncate -s 64M "$work/base.img"
    mkfs.ext4 -q -F -b 1024 -g 1024 -O "$features" "$work/base.img"
    overhead=$(dumpe2fs -h "$work/base.img" 2>/dev/null | sed -n 's/^Overhead clusters:[[:space:]]*//p')
    "$work/probe" "$work/base.img" seed
    for mode in stats stats-ro; do
        cp "$work/base.img" "$work/test.img"
        "$work/probe" "$work/test.img" "$mode" "$overhead"
        check_fs "$work/test.img"
    done
    printf 'PASS: geometry %s\n' "$features"
done
