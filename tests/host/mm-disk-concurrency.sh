#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
CACHE_HOST_BUILD=${CACHE_HOST_BUILD:-build/host/mm-disk} CACHE_HOST_SHIFTS=${CACHE_HOST_SHIFTS:-12} CACHE_HOST_FLAGS="-DCACHE_MM_CASES -DMM_TEST_ELF_SOURCE=1" \
CACHE_HOST_LDFLAGS='-Wl,--wrap=kernel_raw_lock_acquire,--wrap=ext4_ftruncate,--wrap=physical_page_allocate,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_lock_release,--wrap=kernel_page_cache_alias_attach,--wrap=kernel_vma_set_backing_in_use' \
CACHE_HOST_SOURCES='kernel/read_source.c kernel/elf64.c kernel/elf64_source.c fs/open_file.c kernel/namespace.c fs/block_file.c mm/vma.c mm/hugetlb.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c' \
    sh "$root/tests/host/page-cache-concurrency.sh" "$@"
