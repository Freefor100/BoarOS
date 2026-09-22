#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 ${RECLAIM_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_reclaim.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
check_fs() {
    e2fsck -fn "$1" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
}
for blocksize in ${RECLAIM_BLOCK_SIZES:-1024 4096}; do
for format in ${RECLAIM_FORMATS:-file chain}; do
for mapping in ${RECLAIM_MAPPINGS:-extent indirect}; do
    features=orphan_file
    [ "$format" != chain ] || features=^orphan_file
    [ "$mapping" != indirect ] || features="$features,^extent,^64bit"
    truncate -s 32M "$work/base.img"
    mkfs.ext4 -q -F -b "$blocksize" -O "$features" "$work/base.img"
    timeout 60 "$work/probe" "$work/base.img" setup "$work/facts" "${RECLAIM_LAYOUT:-sparse}"
    check_fs "$work/base.img"
    for action in ${RECLAIM_ACTIONS:-unlink truncate}; do
        printf 'RUN: %s-byte %s %s %s\n' "$blocksize" "$format" "$mapping" "$action"
        cp "$work/base.img" "$work/pending.img"
        mutation=$(timeout 60 "$work/probe" "$work/pending.img" mutate "$work/facts" "$action")
        timeout 30 "$work/probe" "$work/pending.img" readonly-log "$work/facts" "$action"
        cp "$work/base.img" "$work/published.img"
        timeout 60 "$work/probe" "$work/published.img" publish "$work/facts" "$action" >/dev/null
        timeout 30 "$work/probe" "$work/published.img" readonly-orphan "$work/facts" "$action"
        cp "$work/pending.img" "$work/recovered.img"
        recovery=$(timeout 60 "$work/probe" "$work/recovered.img" recover "$work/facts" "$action")
        timeout 60 "$work/probe" "$work/recovered.img" recover "$work/facts" "$action" >/dev/null
        check_fs "$work/recovered.img"
        for phase in mutate recover; do
            source="$work/base.img"; total=$mutation; verify=verify
            if [ "$phase" = recover ]; then source="$work/pending.img"; total=$recovery; verify=recover; fi
            for reorder in 0 1; do
                event=1
                while [ "$event" -le "$total" ]; do
                    cp "$source" "$work/cut.img"
                    status=0
                    timeout 60 "$work/probe" "$work/cut.img" "$phase" "$work/facts" "$action" "$event" "$reorder" >/dev/null || status=$?
                    [ "$status" -eq 75 ] || { echo "missing cut: $phase $event status=$status" >&2; exit 1; }
                    timeout 60 "$work/probe" "$work/cut.img" "$verify" "$work/facts" "$action" >/dev/null || {
                        printf 'verification failed after %s cut %s reorder %s\n' "$phase" "$event" "$reorder" >&2
                        exit 1
                    }
                    timeout 60 "$work/probe" "$work/cut.img" "$verify" "$work/facts" "$action" >/dev/null || {
                        printf 'verification failed after %s cut %s reorder %s\n' "$phase" "$event" "$reorder" >&2
                        exit 1
                    }
                    check_fs "$work/cut.img"
                    event=$((event+1))
                done
            done
        done
        printf 'PASS: %s-byte %s %s %s: %s mutation + %s recovery cuts, lost/reordered sectors\n' \
            "$blocksize" "$format" "$mapping" "$action" "$mutation" "$recovery"
    done
done
done
done
