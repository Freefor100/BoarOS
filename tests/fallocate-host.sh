#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
CACHE_HOST_BUILD=${CACHE_HOST_BUILD:-build/host/fallocate} \
CACHE_HOST_TEST_SOURCE=tests/host/fallocate.c CACHE_HOST_FLAGS=-DCACHE_MM_CASES \
CACHE_HOST_LDFLAGS='-Wl,--wrap=kernel_raw_lock_acquire,--wrap=ext4_ftruncate,--wrap=kernel_mm_resolve_user_fault,--wrap=ext4_extent_reserve_range,--wrap=ext4_extent_remove_space' \
CACHE_HOST_SOURCES='fs/open_file.c fs/block_file.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c' \
    sh "$root/tests/page-cache-concurrency-host.sh" fallocate
