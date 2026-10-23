#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${CREDENTIALS_HOST_BUILD:-build/host/credentials}
mkdir -p "$build_dir"
for shift in ${CREDENTIALS_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=4 -DBOAROS_LOCK_DEBUG=1 \
        -Itests/host/sync -idirafter include \
        tests/host/credentials.c kernel/credentials.c kernel/syscall/credentials.c \
        kernel/syscall/capability.c kernel/exec_credentials.c kernel/raw_lock.c -o "$build_dir/credentials-$shift"
    timeout 15s "$build_dir/credentials-$shift"
done
