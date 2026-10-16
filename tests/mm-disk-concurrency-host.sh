#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
CACHE_HOST_BUILD=${CACHE_HOST_BUILD:-build/host/mm-disk} CACHE_HOST_SHIFTS=12 CACHE_HOST_FLAGS=-DCACHE_MM_CASES \
CACHE_HOST_LDFLAGS='-Wl,--wrap=kernel_raw_lock_acquire,--wrap=ext4_ftruncate,--wrap=physical_page_allocate,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_mm_resolve_user_fault' \
CACHE_HOST_SOURCES='fs/open_file.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c' \
    sh "$root/tests/page-cache-concurrency-host.sh"
