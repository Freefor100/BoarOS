#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/mqueue-pipe-20261028/netlink} \
FILE_IO_HOST_TCP=1 FILE_IO_HOST_SHIFTS=${FILE_IO_HOST_SHIFTS:-12} \
FILE_IO_HOST_TEST_SOURCE=tests/host/netlink_notify.c \
FILE_IO_HOST_SOURCES="kernel/pid.c kernel/sched/identity.c ${FILE_IO_HOST_SOURCES:-}" \
FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_open_file_release" \
sh "$root/tests/file-io-concurrency-host.sh"
