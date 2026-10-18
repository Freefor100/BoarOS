#!/bin/sh
set -eu
case "${1:-all}" in all|data|inode) ;; *) echo 'usage: lwext4-lazy-groups-host.sh [all|data|inode]' >&2; exit 2;; esac
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${LAZY_HOST_BUILD:-build/host/lwext4-lazy-groups}
mkdir -p "$build_dir"
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 -DCONFIG_DEBUG_ASSERT=0 \
    -DTEST_RANGE_WORK_COUNTERS=1 -Wl,--wrap=ext4_crc32c -Wl,--wrap=ext4_fs_get_inode_dblk_range -Wl,--wrap=ext4_trans_block_get \
    -include tests/host/lwext4_memory.h -Ithird_party/lwext4/include -idirafter include \
    third_party/lwext4/src/*.c tests/host/lwext4_lazy_groups.c tests/host/block_fault.c \
    kernel/block.c tests/host/block_runtime.c -Itests/host/block_model -pthread -o "$build_dir/probe"
for block in ${LAZY_HOST_BLOCKS:-1024 4096}; do
    for journal in ${LAZY_HOST_JOURNALS:-journal nonjournal}; do
        features=has_journal
        [ "$journal" = journal ] || features=^has_journal
        truncate -s 384M "$build_dir/base.img"
        mkfs.ext4 -q -F -b "$block" -O "$features" \
            -E nodiscard,lazy_itable_init=1,lazy_journal_init=0 "$build_dir/base.img"
        cases=${1:-all}
        [ "$cases" != all ] || cases='data inode'
        for scenario in $cases; do
            image="$build_dir/$scenario-$block-$journal.img"
            cp --reflink=auto "$build_dir/base.img" "$image"
            timeout 30s "$build_dir/probe" "$image" "$scenario" > "$image.log" 2>&1 || { cat "$image.log"; exit 1; }
            cat "$image.log"
            e2fsck -fn "$image" > "$image.fsck.log" 2>&1 || { cat "$image.fsck.log"; exit 1; }
            printf 'PASS lazy groups fs_block=%s %s case=%s; e2fsck clean\n' "$block" "$journal" "$scenario"
            if [ "${LAZY_HOST_CUTS:-0}" = 1 ] && [ "$journal" = journal ] && [ "$scenario" = inode ]; then
                boundaries=$(sed -n 's/^lazy boundaries=//p' "$image.log")
                cut=1
                while [ "$cut" -le "$boundaries" ]; do
                    cp --reflink=auto "$build_dir/base.img" "$build_dir/cut.img"
                    timeout 30s "$build_dir/probe" "$build_dir/cut.img" inode "$cut" > "$build_dir/cut.log" 2>&1 || { cat "$build_dir/cut.log"; exit 1; }
                    timeout 30s "$build_dir/probe" "$build_dir/cut.img" verify > "$build_dir/recovery.log" 2>&1 || { cat "$build_dir/recovery.log"; exit 1; }
                    e2fsck -fn "$build_dir/cut.img" > "$build_dir/cut.fsck.log" 2>&1 || { cat "$build_dir/cut.fsck.log"; exit 1; }
                    cut=$((cut+1))
                done
                printf 'PASS lazy inode fs_block=%s: %s write/flush power cuts, recovered slots/flags/content and e2fsck\n' "$block" "$boundaries"
            fi
        done
    done
done
