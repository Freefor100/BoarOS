#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${REMOUNT_HOST_BUILD:-build/host/remount}
MOUNT_ATTRIBUTES_HOST_BUILD="$build_dir/memory" sh tests/mount-attributes-host.sh
REMOUNT_WORKER_FAILURE=1 FILE_IO_HOST_LDFLAGS=-Wl,--wrap=kernel_thread_create_joinable,--wrap=jbd_memory_set_account \
    FILE_IO_HOST_TEST_SOURCE=tests/host/remount_backend.c FILE_IO_HOST_TCP=0 \
    FILE_IO_HOST_BUILD="$build_dir/ext4" sh tests/file-io-concurrency-host.sh
for shift in 12 14; do
    truncate -s 32M "$build_dir/ext4/fault-$shift.img"
    mkfs.ext4 -q -F -b 4096 "$build_dir/ext4/fault-$shift.img"
    timeout 30s "$build_dir/ext4/concurrency-$shift" "$build_dir/ext4/fault-$shift.img" fault
    truncate -s 32M "$build_dir/ext4/start-fault-$shift.img"
    mkfs.ext4 -q -F -b 4096 "$build_dir/ext4/start-fault-$shift.img"
    timeout 30s "$build_dir/ext4/concurrency-$shift" "$build_dir/ext4/start-fault-$shift.img" start-fault
    truncate -s 32M "$build_dir/ext4/features-$shift.img"
    mkfs.ext4 -q -F -b 4096 -O quota "$build_dir/ext4/features-$shift.img"
    REMOUNT_UNSUPPORTED_RO=1 timeout 30s "$build_dir/ext4/concurrency-$shift" "$build_dir/ext4/features-$shift.img"
    e2fsck -fn "$build_dir/ext4/features-$shift.img" > "$build_dir/ext4/features-fsck-$shift.log" 2>&1
done
FAT_HOST_BUILD="$build_dir/fat" FAT_HOST_CLUSTERS=1 sh tests/fat32-host.sh
