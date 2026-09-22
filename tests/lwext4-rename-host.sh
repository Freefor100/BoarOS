#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 ${RENAME_TEST_CFLAGS:-} \
    -include "$root/tests/host/lwext4_memory.h" \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_rename.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
check_fs() {
    e2fsck -fn "$1" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
}
for size in 1024 4096; do
    for directory in dir_index '^dir_index'; do
        for orphan in orphan_file '^orphan_file'; do
            truncate -s 32M "$work/base.img"
            mkfs.ext4 -q -F -b "$size" -O "$directory,$orphan" "$work/base.img"
            timeout 30 "$work/probe" "$work/base.img" seed
            for mode in basic hardlink wide readonly; do
                cp "$work/base.img" "$work/test.img"
                timeout 30 "$work/probe" "$work/test.img" "$mode"
                check_fs "$work/test.img"
            done
            for damage in parent entry leaf; do
                cp "$work/base.img" "$work/damaged.img"
                timeout 30 "$work/probe" "$work/damaged.img" damage "$damage"
                timeout 30 "$work/probe" "$work/damaged.img" reject "$damage"
            done
            truncate -s 32M "$work/split.img"
            mkfs.ext4 -q -F -b "$size" -O "$directory,$orphan" "$work/split.img"
            timeout 30 "$work/probe" "$work/split.img" seed-split
            for kind in file dir insert split; do
                base="$work/base.img"
                [ "$kind" != split ] || base="$work/split.img"
                for fault in write flush; do
                    for boundary in 1 2; do
                        cp "$base" "$work/test.img"
                        timeout 30 "$work/probe" "$work/test.img" "io-$fault" "$kind" "$boundary"
                        timeout 30 "$work/probe" "$work/test.img" verify "$kind"
                        check_fs "$work/test.img"
                        [ "$fault" != write ] || break
                    done
                done
                cp "$base" "$work/test.img"
                total=$(timeout 30 "$work/probe" "$work/test.img" mutate "$kind")
                timeout 30 "$work/probe" "$work/test.img" verify "$kind"
                check_fs "$work/test.img"
                for order in 0 1; do
                    event=1
                    while [ "$event" -le "$total" ]; do
                        cp "$base" "$work/test.img"
                        status=0
                        timeout 30 "$work/probe" "$work/test.img" mutate "$kind" "$event" "$order" > /dev/null || status=$?
                        [ "$status" -eq 75 ]
                        timeout 30 "$work/probe" "$work/test.img" verify "$kind"
                        timeout 30 "$work/probe" "$work/test.img" verify "$kind"
                        check_fs "$work/test.img"
                        event=$((event + 1))
                    done
                done
                cp "$base" "$work/test.img"
                allocations=$(timeout 30 "$work/probe" "$work/test.img" oom "$kind")
                allocation=1
                while [ "$allocation" -le "$allocations" ]; do
                    cp "$base" "$work/test.img"
                    timeout 30 "$work/probe" "$work/test.img" oom "$kind" "$allocation" > /dev/null
                    timeout 30 "$work/probe" "$work/test.img" verify "$kind"
                    check_fs "$work/test.img"
                    allocation=$((allocation + 1))
                done
                printf 'PASS: %s %s %s %s: %s power cuts x2, %s allocation failures\n' "$size" "$directory" "$orphan" "$kind" "$total" "$allocations"
            done
        done
    done
done
