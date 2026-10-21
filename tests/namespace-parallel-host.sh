#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/namespace-parallel} \
FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_FLAGS="${FILE_IO_HOST_FLAGS:-} -DNAMESPACE_PARALLEL_CASES" \
FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_file_mapping_init,--wrap=kernel_heap_allocate_zeroed,--wrap=ext4_fraw_inode_fill,--wrap=ext4_file_set_times,--wrap=ext4_fs_get_inode_ref" \
sh "$root/tests/file-io-concurrency-host.sh"
