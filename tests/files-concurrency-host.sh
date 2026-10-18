#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${FILES_HOST_BUILD:-build/host/files}
mkdir -p "$build_dir"
for shift in ${FILES_HOST_SHIFTS:-12 14}; do
    # 此成本探针必须观察真实copy调用；Ubuntu的FORTIFY内联绕过--wrap。
    # 仅被测内核table使用非强化声明，宿主fixture及其余对象保留原编译策略。
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin-memcpy \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 ${FILES_HOST_FLAGS:-} \
        -U_FORTIFY_SOURCE -Itests/host/mm -Itests/host/sync -idirafter include \
        -c fs/files/table.c -o "$build_dir/table-$shift.o"
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin-memcpy \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 -DFILES_PIPE_CASES -DCACHE_MM_CASES ${FILES_HOST_FLAGS:-} \
        -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 -DCONFIG_DEBUG_ASSERT=0 \
        -include tests/host/lwext4_memory.h -Wno-unused-but-set-variable -Wno-stringop-truncation \
        -Ithird_party/lwext4/include -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/files_concurrency.c tests/host/mm/context.c tests/host/mm/disk_context.c \
        kernel/cpu.c kernel/raw_lock.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c mm/memory_object.c \
        mm/mm.c mm/vma.c mm/uaccess.c arch/riscv/sv39.c \
        "$build_dir/table-$shift.o" fs/files/path.c fs/files/metadata.c fs/fs_context.c fs/open_file.c fs/block_file.c fs/disk_mount.c fs/pipe.c fs/vfs.c fs/tmpfs.c fs/file_mapping.c \
        fs/record_lock.c fs/page_cache.c fs/ext4_backend.c fs/lwext4_port.c \
        kernel/block.c third_party/lwext4/src/*.c \
        ${FILES_HOST_LDFLAGS:-} -Wl,--gc-sections,--wrap=kernel_open_file_acquire,--wrap=kernel_open_file_release,--wrap=kernel_vfs_path_acquire,--wrap=kernel_heap_allocate_zeroed,--wrap=memcpy,--wrap=kernel_raw_lock_acquire -o "$build_dir/concurrency-$shift"
    timeout 30s "$build_dir/concurrency-$shift"
done
