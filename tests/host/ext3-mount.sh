#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build_dir=${EXT3_MOUNT_HOST_BUILD:-build/host/ext3-mount}
FILE_IO_HOST_TEST_SOURCE=tests/host/ext3_mount.c FILE_IO_HOST_TCP=0 FILE_IO_HOST_FLAGS="-include string.h" \
    FILE_IO_HOST_BUILD="$build_dir" FILE_IO_HOST_BLOCK_SIZE=1024 \
    FILE_IO_HOST_MKFS_ARGS="-t ext3" sh tests/host/file-io-concurrency.sh
for shift in ${FILE_IO_HOST_SHIFTS:-12 14}; do
    for kind in ext2 ext4 ext3; do
        image="$build_dir/$kind-$shift.img"
        truncate -s 32M "$image"
        mke2fs -q -F -t "$kind" -b 4096 "$image"
        set --
        case "$kind" in ext2|ext4) set -- invalid;; esac
        timeout 30s "$build_dir/concurrency-$shift" "$image" "$@"
        e2fsck -fn "$image" > "$build_dir/$kind-fsck-$shift.log" 2>&1
    done
    image="$build_dir/dirty-$shift.img"
    cp "$build_dir/ext3-$shift.img" "$image"
    debugfs -w -R 'feature needs_recovery' "$image" > "$build_dir/dirty-debugfs-$shift.log" 2>&1
    cp "$image" "$build_dir/dirty-before-$shift.img"
    timeout 30s "$build_dir/concurrency-$shift" "$image" dirty
    cmp "$build_dir/dirty-before-$shift.img" "$image"
    rm "$image" "$build_dir/dirty-before-$shift.img"
done
