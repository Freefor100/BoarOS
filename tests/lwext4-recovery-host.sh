#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 ${RECOVERY_TEST_CFLAGS:-} \
    -include "$root/tests/host/lwext4_memory.h" \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_recovery.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
for blocksize in 1024 4096; do
    truncate -s 32M "$work/base.img"
    mkfs.ext4 -q -F -b "$blocksize" "$work/base.img"
    cp "$work/base.img" "$work/regrow.img"
    timeout 30 "$work/probe" "$work/regrow.img" shrink-regrow
    e2fsck -fn "$work/regrow.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    truncate -s 32M "$work/regrow-legacy.img"
    mkfs.ext4 -q -F -b "$blocksize" -O '^extent,^64bit' "$work/regrow-legacy.img"
    timeout 30 "$work/probe" "$work/regrow-legacy.img" shrink-regrow
    e2fsck -fn "$work/regrow-legacy.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    for features in 'extent,orphan_file' 'extent,^orphan_file' \
                    '^extent,^64bit,orphan_file' '^extent,^64bit,^orphan_file'; do
        truncate -s 32M "$work/group.img"
        mkfs.ext4 -q -F -b "$blocksize" -O "$features" "$work/group.img"
        timeout 30 "$work/probe" "$work/group.img" group-remove
        e2fsck -fn "$work/group.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    done
    for regrow_mode in regrow-oom regrow-nested-oom; do
        cp "$work/base.img" "$work/regrow-oom.img"
        total_regrow=$(timeout 30 "$work/probe" "$work/regrow-oom.img" "$regrow_mode")
        allocation=1
        while [ "$allocation" -le "$total_regrow" ]; do
            cp "$work/base.img" "$work/regrow-oom.img"
            timeout 30 "$work/probe" "$work/regrow-oom.img" "$regrow_mode" "$allocation" > /dev/null
            e2fsck -fn "$work/regrow-oom.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
            allocation=$((allocation + 1))
        done
        printf 'PASS: %s-byte %s, %s allocation failure points\n' "$blocksize" "$regrow_mode" "$total_regrow"
    done
    cp "$work/base.img" "$work/corrupt.img"
    timeout 30 "$work/probe" "$work/corrupt.img" damage-root
    timeout 30 "$work/probe" "$work/corrupt.img" reject-root
    cp "$work/base.img" "$work/corrupt-index.img"
    timeout 30 "$work/probe" "$work/corrupt-index.img" commit > /dev/null
    timeout 30 "$work/probe" "$work/corrupt-index.img" damage-index
    timeout 30 "$work/probe" "$work/corrupt-index.img" reject-index
    for mode in abort error commit; do
        cp "$work/base.img" "$work/test.img"
        timeout 30 "$work/probe" "$work/test.img" "$mode" > "$work/events"
        timeout 30 "$work/probe" "$work/test.img" verify
        e2fsck -fn "$work/test.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    done
    total=$(cat "$work/events")
    for reorder in 0 1; do
        event=1
        while [ "$event" -le "$total" ]; do
            cp "$work/base.img" "$work/cut.img"
            status=0
            timeout 30 "$work/probe" "$work/cut.img" commit "$event" "$reorder" || status=$?
            [ "$status" -eq 75 ]
            timeout 30 "$work/probe" "$work/cut.img" verify
            timeout 30 "$work/probe" "$work/cut.img" verify
            e2fsck -fn "$work/cut.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
            event=$((event + 1))
        done
    done
    printf 'PASS: %s-byte filesystem, %s transaction cut points, lost/reordered writes\n' "$blocksize" "$total"
    cp "$work/base.img" "$work/oom.img"
    timeout 30 "$work/probe" "$work/oom.img" oom > "$work/allocations"
    total_allocations=$(cat "$work/allocations")
    allocation=1
    while [ "$allocation" -le "$total_allocations" ]; do
        cp "$work/base.img" "$work/oom.img"
        timeout 30 "$work/probe" "$work/oom.img" oom "$allocation" > /dev/null
        timeout 30 "$work/probe" "$work/oom.img" verify
        e2fsck -fn "$work/oom.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
        allocation=$((allocation + 1))
    done
    printf 'PASS: %s-byte filesystem, %s allocation failure points\n' "$blocksize" "$total_allocations"
done
