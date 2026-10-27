#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
CACHE_HOST_BUILD=${CACHE_HOST_BUILD:-build/host/fallocate} \
CACHE_HOST_TEST_SOURCE=tests/host/fallocate.c CACHE_HOST_FLAGS='-DCACHE_MM_CASES -DFILES_PIPE_CASES' \
CACHE_HOST_LDFLAGS='-Wl,--wrap=kernel_raw_lock_acquire,--wrap=ext4_ftruncate,--wrap=kernel_mm_resolve_user_fault,--wrap=kernel_uaccess_pin_user_page,--wrap=ext4_fs_get_inode_dblk_range,--wrap=ext4_extent_reserve_range,--wrap=ext4_extent_remove_space,--wrap=ext4_blocks_set_direct,--wrap=ext4_file_complete_write,--wrap=kernel_page_cache_prepare_mapping_write,--wrap=ext4_transaction_begin,--wrap=ext4_transaction_end,--wrap=ext4_fopen2,--wrap=ext4_create_child,--wrap=ext4_xattr_get,--wrap=ext4_xattr_remove' \
CACHE_HOST_SOURCES='fs/open_file.c fs/block_file.c fs/pipe.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c' \
    sh "$root/tests/page-cache-concurrency-host.sh" fallocate
