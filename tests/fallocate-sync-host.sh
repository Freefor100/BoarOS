#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/fallocate-sync} \
FILE_IO_HOST_TEST_SOURCE=tests/host/fallocate_sync.c FILE_IO_HOST_TCP=0 \
FILE_IO_HOST_FLAGS=-DFALLOCATE_SYNC_CASES \
FILE_IO_HOST_LDFLAGS=-Wl,--wrap=jbd_journal_accept_state_held \
FILE_IO_HOST_SOURCES=fs/files/range.c sh "$root/tests/file-io-concurrency-host.sh"
