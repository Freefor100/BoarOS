#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${FILE_IO_HOST_BUILD:-build/host/ext4-namespace-parallel}
mkfs_args=${FILE_IO_HOST_MKFS_ARGS:--g 1024 -N 1024}
FILE_IO_HOST_BUILD="$build_dir" \
FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_TEST_SOURCE=tests/host/ext4_namespace_parallel.c \
FILE_IO_HOST_MKFS_ARGS="$mkfs_args" \
FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=jbd_trans_set_block_dirty_range,--wrap=jbd_trans_set_block_dirty,--wrap=jbd_journal_checkpoint,--wrap=kernel_thread_create_joinable,--wrap=ext4_block_get,--wrap=jbd_journal_free_trans_state_held,--wrap=kernel_time_monotonic_ns,--wrap=kernel_wait_queue_wake_all,--wrap=kernel_wait_park,--wrap=jbd_journal_submit,--wrap=jbd_trans_get_write_access_range,--wrap=jbd_trans_prepare_operation,--wrap=ext4_user_malloc,--wrap=ext4_user_calloc" \
sh "$root/tests/file-io-concurrency-host.sh"
cd "$root"
for shift in ${FILE_IO_HOST_SHIFTS:-12 14}; do
    truncate -s 32M "$build_dir/worker-image-$shift.img"
    mkfs.ext4 -q -F -b "${FILE_IO_HOST_BLOCK_SIZE:-4096}" $mkfs_args "$build_dir/worker-image-$shift.img"
    EXT4_NAMESPACE_PARALLEL_WORKERS=1 timeout 30s "$build_dir/concurrency-$shift" "$build_dir/worker-image-$shift.img"
    e2fsck -fn "$build_dir/worker-image-$shift.img" > "$build_dir/worker-fsck-$shift.log" 2>&1
done
