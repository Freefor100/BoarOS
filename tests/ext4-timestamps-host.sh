#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${FILE_IO_HOST_BUILD:-build/host/ext4-timestamps}
FILE_IO_HOST_BUILD="$build_dir" \
FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_TEST_SOURCE=tests/host/ext4_timestamps.c \
sh "$root/tests/file-io-concurrency-host.sh"
cd "$root"
for shift in ${FILE_IO_HOST_SHIFTS:-12 14}; do
    truncate -s 32M "$build_dir/image-1k-$shift.img"
    mkfs.ext4 -q -F -b 1024 "$build_dir/image-1k-$shift.img"
    timeout 30s "$build_dir/concurrency-$shift" "$build_dir/image-1k-$shift.img"
    e2fsck -fn "$build_dir/image-1k-$shift.img" > "$build_dir/fsck-1k-$shift.log" 2>&1
    truncate -s 32M "$build_dir/image-remount-$shift.img"
    mkfs.ext4 -q -F -b 4096 -O ^has_journal "$build_dir/image-remount-$shift.img"
    debugfs -w -R "write /dev/null timestamp" "$build_dir/image-remount-$shift.img" > "$build_dir/debugfs-remount-$shift.log" 2>&1
    timeout 30s "$build_dir/concurrency-$shift" "$build_dir/image-remount-$shift.img" remount
    e2fsck -fn "$build_dir/image-remount-$shift.img" > "$build_dir/fsck-remount-$shift.log" 2>&1
done
