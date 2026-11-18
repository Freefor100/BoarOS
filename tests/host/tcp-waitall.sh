#!/bin/sh
set -eu
export FILE_IO_HOST_TEST_SOURCE=tests/host/tcp_waitall.c
export FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/tcp-waitall}
exec sh "$(dirname "$0")/file-io-concurrency.sh"
