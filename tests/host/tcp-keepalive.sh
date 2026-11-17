#!/bin/sh
set -eu
export FILE_IO_HOST_TEST_SOURCE=tests/host/tcp_keepalive.c
export FILE_IO_HOST_BUILD=${FILE_IO_HOST_BUILD:-build/host/tcp-keepalive}
exec sh "$(dirname "$0")/file-io-concurrency.sh"
