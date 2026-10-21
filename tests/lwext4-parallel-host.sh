#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
source_root=${PARALLEL_SOURCE_ROOT:-$root}
work=${PARALLEL_HOST_BUILD:-$root/build/host/lwext4-parallel}
mkdir -p "$work"
cc=${HOST_CC:-cc}
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-but-set-variable \
    -Wno-stringop-truncation -DBOAROS_MAX_CPUS=2 -DCONFIG_USE_DEFAULT_CFG=1 \
    -I"$source_root/third_party/lwext4/include" -idirafter "$source_root/include" \
    "$source_root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_journal.c" \
    "$source_root/tests/host/block_fault.c" "$source_root/kernel/block.c" \
    "$source_root/tests/host/block_runtime.c" "$source_root/kernel/raw_lock.c" \
    "$source_root/tests/host/sync/notifications.c" \
    -I"$source_root/tests/host/block_model" -pthread -o "$work/probe"
printf 'Old data' > "$work/data"
for block in 1024 4096; do
    image="$work/private-views-$block.img"
    truncate -s 32M "$image"
    mkfs.ext4 -q -F -b "$block" "$image"
    debugfs -w -R "write $work/data /probe" "$image" >/dev/null 2>&1
    timeout 30 "$work/probe" "$image" private-views
    e2fsck -fn "$image" > "$work/fsck-$block.log" 2>&1
    if rg -i 'wrong|differences:|corrupt|warning|UNEXPECTED' "$work/fsck-$block.log"; then
        cat "$work/fsck-$block.log" >&2
        exit 1
    fi
done
