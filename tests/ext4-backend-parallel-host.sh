#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${FILE_IO_HOST_BUILD:-build/host/ext4-backend-parallel}
FILE_IO_HOST_BUILD="$build_dir" \
FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_TEST_SOURCE=tests/host/ext4_backend_parallel.c \
FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=jbd_trans_set_block_dirty_range,--wrap=jbd_journal_checkpoint,--wrap=kernel_thread_create_joinable" \
sh "$root/tests/file-io-concurrency-host.sh"
cd "$root"
for shift in ${FILE_IO_HOST_SHIFTS:-12 14}; do
    truncate -s 32M "$build_dir/image-1k-$shift.img"
    mkfs.ext4 -q -F -b 1024 ${FILE_IO_HOST_MKFS_ARGS:-} "$build_dir/image-1k-$shift.img"
    timeout 30s "$build_dir/concurrency-$shift" "$build_dir/image-1k-$shift.img"
    e2fsck -fn "$build_dir/image-1k-$shift.img" > "$build_dir/fsck-1k-$shift.log" 2>&1
done
