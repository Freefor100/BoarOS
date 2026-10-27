#!/bin/sh
set -eu
export FILE_IO_HOST_TEST_SOURCE=tests/host/xattr.c
export FILE_IO_HOST_TCP=0
export FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_heap_allocate_zeroed,--wrap=ext4_xattr_set"
export FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/xattr}
exec sh "$(dirname "$0")/file-io-concurrency-host.sh"
