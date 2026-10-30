#!/bin/sh
set -eu
FILE_IO_HOST_BUILD=${WRITE_TARGET_HOST_BUILD:-build/host/write-target} \
FILE_IO_HOST_TEST_SOURCE=tests/host/write_target.c FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_vfs_node_pread_batch" \
sh "$(dirname -- "$0")/file-io-concurrency-host.sh"
