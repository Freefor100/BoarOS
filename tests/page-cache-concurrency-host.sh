#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
mkdir -p build/host/cache
for shift in 12 14; do
    ${HOST_CC:-cc} -std=gnu11 ${CACHE_HOST_OPT:--O2} -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 \
        -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 -DCONFIG_DEBUG_ASSERT=0 \
        -include tests/host/lwext4_memory.h \
        -Wno-unused-but-set-variable -Wno-stringop-truncation -Ithird_party/lwext4/include \
        -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/page_cache_concurrency.c tests/host/mm/context.c \
        tests/host/mm/disk_context.c kernel/cpu.c kernel/raw_lock.c \
        kernel/sched/sync.c kernel/sched/park.c kernel/physical_page.c \
        mm/heap.c mm/memory_object.c fs/vfs.c fs/tmpfs.c fs/file_mapping.c \
        fs/record_lock.c fs/page_cache.c fs/ext4_backend.c fs/lwext4_port.c \
        kernel/block.c third_party/lwext4/src/*.c \
        -Wl,--gc-sections,--wrap=kernel_vfs_node_pread,--wrap=kernel_thread_join \
        -o "build/host/cache/concurrency-$shift"
    truncate -s 32M "build/host/cache/image-$shift.img"
    mkfs.ext4 -q -F -b 4096 "build/host/cache/image-$shift.img"
    timeout 30s "build/host/cache/concurrency-$shift" "build/host/cache/image-$shift.img"
    e2fsck -fn "build/host/cache/image-$shift.img" > "build/host/cache/fsck-$shift.log" 2>&1
done
