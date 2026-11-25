#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
case "${1:-all}" in
    all) ;;
    oom-order) export FAULT_WINDOW_CASE=oom-order ;;
    hot-lookup) export FAULT_WINDOW_CASE=hot-lookup ;;
    *) echo 'usage: fault-read-window.sh [all|oom-order|hot-lookup]' >&2; exit 2 ;;
esac
CACHE_HOST_BUILD=${CACHE_HOST_BUILD:-build/host/fault-read-window} \
CACHE_HOST_TEST_SOURCE=tests/host/fault_read_window.c \
CACHE_HOST_FLAGS="${CACHE_HOST_FLAGS:-} -DCACHE_MM_CASES -DFILES_PIPE_CASES -DMM_TEST_ELF_SOURCE=1" \
CACHE_HOST_LDFLAGS="${CACHE_HOST_LDFLAGS:-} -Wl,--wrap=kernel_page_cache_get_for_fault,--wrap=kernel_open_file_try_prepare_disk_mapping_page,--wrap=kernel_heap_allocate_nosleep,--wrap=kernel_heap_allocate_zeroed_nosleep,--wrap=kernel_heap_allocate_zeroed_batch_nosleep,--wrap=riscv_smp_sync_user" \
CACHE_HOST_SOURCES='fs/open_file.c kernel/namespace.c fs/block_file.c fs/pipe.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c kernel/read_source.c kernel/elf64.c kernel/elf64_source.c' \
    sh "$root/tests/host/page-cache-concurrency.sh" read-batch
