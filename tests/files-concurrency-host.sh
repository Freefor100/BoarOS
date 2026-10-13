#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${FILES_HOST_BUILD:-build/host/files}
mkdir -p "$build_dir"
for shift in ${FILES_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin-memcpy \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 -DFILES_PIPE_CASES \
        -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 -DCONFIG_DEBUG_ASSERT=0 \
        -include tests/host/lwext4_memory.h -Wno-unused-but-set-variable -Wno-stringop-truncation \
        -Ithird_party/lwext4/include -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/files_concurrency.c tests/host/mm/context.c tests/host/mm/disk_context.c \
        kernel/cpu.c kernel/raw_lock.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c \
        mm/mm.c mm/vma.c mm/uaccess.c arch/riscv/sv39.c \
        fs/files/table.c fs/open_file.c fs/pipe.c fs/vfs.c fs/tmpfs.c fs/file_mapping.c \
        fs/record_lock.c fs/page_cache.c fs/ext4_backend.c fs/lwext4_port.c \
        kernel/block.c third_party/lwext4/src/*.c \
        -Wl,--gc-sections,--wrap=kernel_open_file_acquire,--wrap=kernel_heap_allocate_zeroed,--wrap=memcpy,--wrap=kernel_raw_lock_acquire -o "$build_dir/concurrency-$shift"
    timeout 30s "$build_dir/concurrency-$shift"
done
