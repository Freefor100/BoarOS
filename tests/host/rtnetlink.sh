#!/bin/sh
set -eu
export FILE_IO_HOST_TEST_SOURCE=tests/host/rtnetlink.c
export FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/rtnetlink}
export FILE_IO_HOST_SOURCES="kernel/pid.c kernel/sched/identity.c ${FILE_IO_HOST_SOURCES:-}"
export FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_open_file_release"
exec sh "$(dirname "$0")/file-io-concurrency.sh"
