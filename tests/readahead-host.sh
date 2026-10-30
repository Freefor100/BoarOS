#!/bin/sh
set -eu
FILE_IO_HOST_BUILD=${READAHEAD_HOST_BUILD:-build/host/readahead} \
FILE_IO_HOST_TEST_SOURCE=tests/host/readahead.c FILE_IO_HOST_TCP=0 \
sh "$(dirname -- "$0")/file-io-concurrency-host.sh"
