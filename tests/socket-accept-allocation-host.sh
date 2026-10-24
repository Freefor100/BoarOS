#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/socket-accept-allocation} \
FILE_IO_HOST_TCP=1 \
FILE_IO_HOST_TEST_SOURCE=tests/host/socket_accept_allocation.c \
FILE_IO_HOST_SOURCES="kernel/credentials.c kernel/pid.c kernel/sched/identity.c ${FILE_IO_HOST_SOURCES:-}" \
FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_heap_allocate_zeroed" \
sh "$root/tests/file-io-concurrency-host.sh"
