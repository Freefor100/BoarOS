#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-but-set-variable \
    -Wno-stringop-truncation -DCONFIG_USE_DEFAULT_CFG=1 ${JOURNAL_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_ordered.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
truncate -s 32M "$work/base.img"
mkfs.ext4 -q -F -b 1024 "$work/base.img"
printf 'Old data' > "$work/data"
debugfs -w -R "write $work/data /probe" "$work/base.img" >/dev/null 2>&1
for action in commit abort write-error flush-error data-only access-only reuse \
              log-write-error log-flush-error log-commit-flush-error; do
    cp "$work/base.img" "$work/test.img"
    timeout 30 "$work/probe" "$work/test.img" "$action"
    if [ "$action" != abort ] && [ "$action" != access-only ]; then
        recover=recover
        case "$action" in data-only|log-*) recover=recover-oldmeta ;; esac
        timeout 30 "$work/probe" "$work/test.img" "$recover"
        e2fsck -fn "$work/test.img" >/dev/null
    fi
done
printf '%s\n' 'PASS: ordered data precedes metadata log, stays outside log, scoped writes, rollback/retry/replay'

# Every start/stop write and barrier, including primary-superblock checkpoints.
for blocksize in 1024 4096; do
truncate -s 32M "$work/lifecycle-base.img"
mkfs.ext4 -q -F -b "$blocksize" "$work/lifecycle-base.img"
cp "$work/lifecycle-base.img" "$work/lifecycle.img"
total=$(timeout 30 "$work/probe" "$work/lifecycle.img" lifecycle)
for reorder in 0 1; do
    event=1
    while [ "$event" -le "$total" ]; do
        cp "$work/lifecycle-base.img" "$work/lifecycle.img"
        status=0
        timeout 30 "$work/probe" "$work/lifecycle.img" lifecycle "$event" "$reorder" || status=$?
        [ "$status" -eq 75 ]
        timeout 30 "$work/probe" "$work/lifecycle.img" recover-lifecycle
        timeout 30 "$work/probe" "$work/lifecycle.img" recover-lifecycle
        e2fsck -fn "$work/lifecycle.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
        event=$((event + 1))
    done
done
printf 'PASS: %s-byte journal start/stop %s cut points with lost/reordered 512-byte sectors\n' "$blocksize" "$total"
done
