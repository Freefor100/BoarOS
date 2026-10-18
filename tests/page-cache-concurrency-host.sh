#!/bin/sh
set -eu
case "${1:-all}" in all|truncate|pressure|pressure-errors|fallocate) ;; *) echo 'usage: page-cache-concurrency-host.sh [all|truncate|pressure|pressure-errors|fallocate]' >&2; exit 2;; esac
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${CACHE_HOST_BUILD:-build/host/cache}
mkdir -p "$build_dir"
for shift in ${CACHE_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 ${CACHE_HOST_OPT:--O2} -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 ${CACHE_HOST_FLAGS:-} \
        -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 -DCONFIG_DEBUG_ASSERT=0 \
        -include tests/host/lwext4_memory.h \
        -Wno-unused-but-set-variable -Wno-stringop-truncation -Ithird_party/lwext4/include \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        ${CACHE_HOST_TEST_SOURCE:-tests/host/page_cache_concurrency.c} tests/host/mm/context.c \
        tests/host/mm/disk_context.c kernel/cpu.c kernel/raw_lock.c \
        kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c \
        mm/heap.c mm/memory_object.c fs/vfs.c fs/tmpfs.c fs/file_mapping.c \
        fs/record_lock.c fs/page_cache.c fs/ext4_backend.c fs/lwext4_port.c \
        kernel/block.c third_party/lwext4/src/*.c ${CACHE_HOST_SOURCES:-} \
        -Wl,--gc-sections,--wrap=kernel_vfs_node_pread,--wrap=kernel_vfs_node_pread_batch,--wrap=physical_page_allocate,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_heap_allocate,--wrap=kernel_vfs_node_writeback,--wrap=ext4_fpwrite_batch,--wrap=kernel_thread_join,--wrap=kernel_open_file_prepare_mapping_page \
        -Wl,--wrap=kernel_rwlock_read,--wrap=kernel_rwlock_write,--wrap=kernel_rwlock_try_write,--wrap=kernel_wait_prepare,--wrap=kernel_wait_park,--wrap=kernel_wait_queue_destroy,--wrap=physical_page_acquire,--wrap=physical_page_release,--wrap=physical_page_available,--wrap=kernel_vfs_sync_complete \
        -Wl,--wrap=ext4_file_writeback_times,--wrap=ext4_file_sync_metadata_mode ${CACHE_HOST_LDFLAGS:-} \
        -o "$build_dir/concurrency-$shift"
    for block in ${CACHE_HOST_BLOCKS:-1024 4096}; do
        if [ "${1:-all}" = fallocate ]; then
            for journal in ${FALLOCATE_HOST_JOURNALS:-journal nonjournal}; do
              for scenario in ${FALLOCATE_HOST_CASES:-${FALLOCATE_HOST_CASE:-fallocate}}; do
                image="$build_dir/$scenario-$shift-$block-$journal.img"
                truncate -s "${FALLOCATE_HOST_IMAGE_SIZE:-32M}" "$image"
                features=has_journal
                [ "$journal" = journal ] || features=^has_journal
                mkfs.ext4 -q -F -b "$block" -O "$features" "$image"
                timeout 30s "$build_dir/concurrency-$shift" "$image" "$scenario"
                e2fsck -fn "$image" > "$build_dir/fsck-$scenario-$shift-$block-$journal.log" 2>&1
                printf 'PASS %s page_shift=%s fs_block=%s %s; e2fsck clean\n' "$scenario" "$shift" "$block" "$journal"
              done
            done
            continue
        fi
        if [ "${1:-all}" = pressure ] || [ "${1:-all}" = pressure-errors ]; then
            scenarios='write-pressure-other-eio write-pressure-other-enospc'
            if [ "${1:-all}" = pressure ]; then scenarios="write-pressure write-pressure-full write-pressure-eio write-pressure-oom $scenarios"; fi
            for scenario in $scenarios; do
                image="$build_dir/$scenario-$shift-$block.img"
                truncate -s 32M "$image"
                mkfs.ext4 -q -F -b "$block" "$image"
                timeout 30s "$build_dir/concurrency-$shift" "$image" "$scenario"
                e2fsck -fn "$image" > "$build_dir/fsck-$scenario-$shift-$block.log" 2>&1
            done
            continue
        fi
        truncate -s 32M "$build_dir/truncate-$shift-$block.img"
        mkfs.ext4 -q -F -b "$block" "$build_dir/truncate-$shift-$block.img"
        timeout 15s "$build_dir/concurrency-$shift" "$build_dir/truncate-$shift-$block.img" truncate-writeback
        e2fsck -fn "$build_dir/truncate-$shift-$block.img" > "$build_dir/fsck-truncate-$shift-$block.log" 2>&1
        if [ "${1:-all}" = truncate ]; then continue; fi
        truncate -s 32M "$build_dir/image-$shift-$block.img"
        mkfs.ext4 -q -F -b "$block" "$build_dir/image-$shift-$block.img"
        timeout 30s "$build_dir/concurrency-$shift" "$build_dir/image-$shift-$block.img"
        e2fsck -fn "$build_dir/image-$shift-$block.img" > "$build_dir/fsck-$shift-$block.log" 2>&1
        truncate -s 32M "$build_dir/error-$shift-$block.img"
        mkfs.ext4 -q -F -b "$block" "$build_dir/error-$shift-$block.img"
        timeout 15s "$build_dir/concurrency-$shift" "$build_dir/error-$shift-$block.img" durable-error
        timeout 15s "$build_dir/concurrency-$shift" "$build_dir/error-$shift-$block.img" durable-recover
        e2fsck -fn "$build_dir/error-$shift-$block.img" > "$build_dir/fsck-error-recovered-$shift-$block.log" 2>&1
    done
done
