#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${FILE_IO_HOST_BUILD:-build/host/file-io}
mkdir -p "$build_dir"
network_sources=
if [ "${FILE_IO_HOST_TCP:-1}" = 1 ]; then
    network_sources=$(python3 -c 'import re; s=open("Makefile").read().split("LWIP_SOURCES :=",1)[1].split("\n\n",1)[0]; print(" ".join(re.findall(r"(?:third_party/lwip|net/lwip_port)/[\w/]+\.c",s)))')
    FILE_IO_HOST_FLAGS="${FILE_IO_HOST_FLAGS:-} -DTCP_IO_CASES -include sys/types.h -include limits.h"
    FILE_IO_HOST_LDFLAGS="${FILE_IO_HOST_LDFLAGS:-} -Wl,--wrap=kernel_wait_park,--wrap=kernel_wait_finish,--wrap=kernel_mm_pin_user_page,--wrap=kernel_socket_read_user,--wrap=kernel_socket_finish_read,--wrap=kernel_socket_stream_commitv"
    FILE_IO_HOST_LDFLAGS="$FILE_IO_HOST_LDFLAGS -Wl,--wrap=kernel_socket_receive_timeout,--wrap=kernel_socket_send_timeout,--wrap=kernel_socket_reserve_read,--wrap=kernel_socket_stream_reserve,--wrap=kernel_socket_write_unix,--wrap=kernel_time_monotonic_ns"
fi
set --
for symbol in $(sed -n 's/^EXCLUDED(\([^)]*\)).*/\1/p' tests/host/file_io_excluded.c); do
    if [ "${FILE_IO_HOST_TCP:-1}" = 1 ]; then
        case "$symbol" in kernel_socket_*|kernel_time_deadline_from_monotonic) continue;; esac
    fi
    set -- "$@" "-Wl,--wrap=$symbol"
done
if [ "${FILE_IO_HOST_TCP:-1}" = 0 ]; then
    set -- "$@" "-Wl,--wrap=kernel_time_deadline_from_monotonic"
fi
for shift in ${FILE_IO_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 \
        -DFILE_IO_CASES -Itests/host/mm -Itests/host/sync -idirafter include \
        -c kernel/sched/process.c -o "$build_dir/process-$shift.o"
    # 调度执行由pthread模型提供；该weak化仅移除未链接的客户机join定义，
    # task定位及scratch acquire/release均使用生产process实现。
    ${HOST_OBJCOPY:-objcopy} --weaken-symbol=kernel_thread_join "$build_dir/process-$shift.o"
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 \
        -DFILE_IO_CASES -DFILES_PIPE_CASES -DCONFIG_USE_DEFAULT_CFG=1 \
        -DCONFIG_USE_USER_MALLOC=1 -DCONFIG_DEBUG_ASSERT=0 \
        -include tests/host/lwext4_memory.h -Wno-unused-but-set-variable -Wno-stringop-truncation \
        -Ithird_party/lwext4/include -Itests/host/mm -Itests/host/sync -idirafter net/lwip_port/include \
        -idirafter third_party/lwip/src/include -idirafter include \
        ${FILE_IO_HOST_TEST_SOURCE:-tests/host/file_io_concurrency.c} tests/host/file_io_excluded.c tests/host/mm/context.c tests/host/mm/disk_context.c \
        kernel/cpu.c kernel/raw_lock.c kernel/credentials.c kernel/sched/sync.c kernel/sched/accounting.c kernel/sched/park.c kernel/sched/futex.c "$build_dir/process-$shift.o" \
        kernel/physical_page.c mm/heap.c mm/memory_object.c mm/mm.c mm/vma.c mm/uaccess.c arch/riscv/sv39.c \
        fs/files/table.c fs/files/io.c fs/files/locks.c fs/files/path.c fs/files/metadata.c fs/files/socket.c fs/files/epoll.c fs/fs_context.c fs/open_file.c fs/block_file.c fs/loop.c fs/disk_mount.c \
        fs/pipe.c fs/vfs.c fs/tmpfs.c fs/file_mapping.c fs/record_lock.c \
        fs/page_cache.c fs/ext4_backend.c fs/fat32.c fs/lwext4_port.c kernel/block.c net/socket.c \
        third_party/lwext4/src/*.c $network_sources ${FILE_IO_HOST_SOURCES:-} ${FILE_IO_HOST_FLAGS:-} \
        ${FILE_IO_HOST_LDFLAGS:-} "$@" -Wl,--gc-sections,--wrap=ext4_fraw_inode_fill,--wrap=kernel_heap_allocate,--wrap=kernel_rwlock_write,--wrap=kernel_lock_held,--wrap=kernel_vfs_sync_borrowing,--wrap=kernel_vfs_sync_range,--wrap=kernel_vfs_file_accessed,--wrap=kernel_record_lock_set,--wrap=kernel_wait_prepare,--wrap=kernel_mm_unpin_user_page,--wrap=riscv_smp_sync_user \
        -o "$build_dir/concurrency-$shift"
    truncate -s 32M "$build_dir/image-$shift.img"
    mkfs.ext4 -q -F -b "${FILE_IO_HOST_BLOCK_SIZE:-4096}" ${FILE_IO_HOST_MKFS_ARGS:-} "$build_dir/image-$shift.img"
    timeout 30s "$build_dir/concurrency-$shift" "$build_dir/image-$shift.img"
    e2fsck -fn "$build_dir/image-$shift.img" > "$build_dir/fsck-$shift.log" 2>&1
done
