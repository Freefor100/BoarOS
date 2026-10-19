#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${FAT_HOST_BUILD:-build/host/fat32}
mkdir -p "$build_dir"
for shift in ${FAT_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_MAX_CPUS=2 \
        -Ithird_party/lwext4/include -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/fat32.c tests/host/mm/context.c tests/host/mm/disk_context.c \
        kernel/cpu.c kernel/raw_lock.c kernel/sched/sync.c kernel/sched/accounting.c kernel/sched/park.c kernel/physical_page.c \
        mm/heap.c mm/memory_object.c fs/vfs.c fs/fat32.c fs/file_mapping.c fs/record_lock.c fs/page_cache.c \
        kernel/block.c -Wl,--gc-sections -o "$build_dir/test-$shift"
    for cluster in ${FAT_HOST_CLUSTERS:-1 8}; do
        image="$build_dir/disk-$shift-$cluster.img"
        truncate -s "$((64 * cluster))M" "$image"
        mkfs.fat -F 32 -s "$cluster" "$image" > "$build_dir/mkfs-$shift-$cluster.log"
        timeout 30s "$build_dir/test-$shift" "$image"
        fsck.fat -vn "$image" > "$build_dir/fsck-$shift-$cluster.log" 2>&1
        printf 'PASS FAT32 page_shift=%s cluster_sectors=%s; fsck.fat clean\n' "$shift" "$cluster"
    done
    image="$build_dir/fault-$shift.img"
    truncate -s 64M "$image"
    mkfs.fat -F 32 -s 1 "$image" > "$build_dir/mkfs-fault-$shift.log"
    timeout 15s "$build_dir/test-$shift" "$image" fault
    if fsck.fat -vn "$image" > "$build_dir/fsck-fault-$shift.log" 2>&1; then
        echo 'failed I/O mount lost its on-disk dirty indication' >&2
        exit 1
    fi
done
