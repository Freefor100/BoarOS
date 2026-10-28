#!/bin/sh
set -eu
FILES_HOST_BUILD=${FILES_HOST_BUILD:-build/host/pipe} FILES_HOST_FLAGS=-DPIPE_CONCURRENCY_CASES FILES_HOST_LDFLAGS='-Wl,--wrap=kernel_wait_park,--wrap=kernel_copy_from_user,--wrap=kernel_copy_to_user' \
    sh "$(dirname "$0")/files-concurrency-host.sh"
