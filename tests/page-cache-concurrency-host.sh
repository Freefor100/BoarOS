#!/bin/sh
set -eu
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
        tests/host/page_cache_concurrency.c tests/host/mm/context.c \
        tests/host/mm/disk_context.c kernel/cpu.c kernel/raw_lock.c \
        kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c \
        mm/heap.c mm/memory_object.c fs/vfs.c fs/tmpfs.c fs/file_mapping.c \
        fs/record_lock.c fs/page_cache.c fs/ext4_backend.c fs/lwext4_port.c \
        kernel/block.c third_party/lwext4/src/*.c ${CACHE_HOST_SOURCES:-} \
        -Wl,--gc-sections,--wrap=kernel_vfs_node_pread,--wrap=kernel_vfs_node_writeback,--wrap=kernel_thread_join,--wrap=kernel_open_file_prepare_mapping_page ${CACHE_HOST_LDFLAGS:-} \
        -o "$build_dir/concurrency-$shift"
    truncate -s 32M "$build_dir/image-$shift.img"
    mkfs.ext4 -q -F -b 4096 "$build_dir/image-$shift.img"
    timeout 30s "$build_dir/concurrency-$shift" "$build_dir/image-$shift.img"
    e2fsck -fn "$build_dir/image-$shift.img" > "$build_dir/fsck-$shift.log" 2>&1
done
