#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DBOAROS_MAX_CPUS=2 -DCONFIG_USE_DEFAULT_CFG=1 -DTEST_TRUNCATE_FALLBACK=1 \
    -Wl,--wrap=ext4_balloc_free_blocks ${TRUNCATE_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_truncate.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" "$root/tests/host/block_runtime.c" "$root/kernel/raw_lock.c" "$root/tests/host/sync/notifications.c" -I"$root/tests/host/block_model" -pthread -o "$work/probe"
check_fs() {
    e2fsck -fn "$1" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
}
for blocksize in 1024 4096; do
    for mapping in extent indirect; do
        features=orphan_file
        [ "$mapping" != indirect ] || features='^extent,^64bit,^orphan_file'
        truncate -s 32M "$work/base.img"
        mkfs.ext4 -q -F -b "$blocksize" -O "$features" "$work/base.img"
        scenarios='aligned partial zero sparse unlinked'
        if [ "$mapping" = extent ]; then scenarios="$scenarios unlinked-dense"; fi
        for scenario in $scenarios; do
            kind=dense
            crashes=0
            target=$((16 * blocksize))
            case "$scenario" in
                partial) target=$((16 * blocksize + 17));;
                zero) target=0;;
                sparse) kind=sparse; target=$((16 * blocksize + 17));;
                unlinked) kind=unlinked; target=0;;
                unlinked-dense) kind=unlinked-dense; target=0;;
            esac
            cp "$work/base.img" "$work/pending.img"
            timeout 60 "$work/probe" "$work/pending.img" prepare "$target" "$kind"
            if [ "$scenario" = unlinked-dense ]; then
                for error in memory space; do
                    cp "$work/pending.img" "$work/fallback.img"
                    timeout 30 "$work/probe" "$work/fallback.img" "$error" "$target" "$kind"
                    check_fs "$work/fallback.img"
                done
            fi
            if [ "$kind" = dense ]; then
                # Each completed step is followed by an actual process restart.
                done=0
                windows=0
                while [ "$done" = 0 ]; do
                    timeout 30 "$work/probe" "$work/pending.img" step "$target" "$kind" > "$work/step"
                    read -r done events < "$work/step"
                    windows=$((windows + 1))
                done
                if [ "$mapping" = extent ]; then
                    keep=$(((target + blocksize - 1) / blocksize))
                    test "$windows" -eq "$(((80 - keep + 31) / 32))"
                fi
            else
                done=0
                while [ "$done" = 0 ]; do
                    cp "$work/pending.img" "$work/stage.img"
                    timeout 30 "$work/probe" "$work/pending.img" step "$target" "$kind" > "$work/step"
                    read -r done events < "$work/step"
                    if [ "$scenario" = unlinked-dense ]; then
                        [ "$done" -eq 1 ] || { echo 'dense orphan requires more than one reclaim transaction' >&2; exit 1; }
                    fi
                    for reorder in 0 1; do
                        event=1
                        while [ "$event" -le "$events" ]; do
                            cp "$work/stage.img" "$work/cut.img"
                            status=0
                            timeout 30 "$work/probe" "$work/cut.img" step "$target" "$kind" "$event" "$reorder" || status=$?
                            [ "$status" -eq 75 ]
                            timeout 60 "$work/probe" "$work/cut.img" finish "$target" "$kind"
                            check_fs "$work/cut.img"
                            crashes=$((crashes + 1))
                            event=$((event + 1))
                        done
                    done
                done
            fi
            timeout 60 "$work/probe" "$work/pending.img" finish "$target" "$kind"
            check_fs "$work/pending.img"
            printf 'PASS: %s-byte %s %s persisted-size reclamation, %s interrupted commits replayed\n' "$blocksize" "$mapping" "$scenario" "$crashes"
        done
    done
done
