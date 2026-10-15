#!/bin/sh
# Exercise real grouped transactions with independent device counters.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
    -include "$root/tests/host/lwext4_memory.h" \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_metadata.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
"$work/probe" bitmap
for block in 1024 4096; do
    truncate -s 32M "$work/disk.img"
    mkfs.ext4 -q -F -b "$block" -I 256 "$work/disk.img"
    "$work/probe" "$work/disk.img" seed
    cp "$work/disk.img" "$work/base.img"
    for mode in group-quarantine group-quarantine-error; do
        cp "$work/base.img" "$work/quarantine.img"
        "$work/probe" "$work/quarantine.img" "$mode"
        if [ "$mode" = group-quarantine-error ]; then
            "$work/probe" "$work/quarantine.img" group-recovery
            "$work/probe" "$work/quarantine.img" group-recovery
        fi
        e2fsck -fn "$work/quarantine.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    done
    for fault in 0 1 2 3 4; do
        cp "$work/base.img" "$work/checkpoint.img"
        "$work/probe" "$work/checkpoint.img" group-checkpoint-collapse "$fault"
        "$work/probe" "$work/checkpoint.img" group-checkpoint-recovery
        "$work/probe" "$work/checkpoint.img" group-checkpoint-recovery
        e2fsck -fn "$work/checkpoint.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    done
    for operation in truncate unlink; do
        cp "$work/base.img" "$work/namespace.img"
        "$work/probe" "$work/namespace.img" "group-namespace-$operation"
        e2fsck -fn "$work/namespace.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    done
    cp "$work/base.img" "$work/bounds.img"
    "$work/probe" "$work/bounds.img" group-log-payload
    e2fsck -fn "$work/bounds.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    cp "$work/base.img" "$work/bounds.img"
    "$work/probe" "$work/bounds.img" group-bounds
    e2fsck -fn "$work/bounds.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    cp "$work/base.img" "$work/bounds.img"
    "$work/probe" "$work/bounds.img" group-borrow-admission
    e2fsck -fn "$work/bounds.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    for fault in 0 1 2; do
        for persist in 0 1 2; do
            cp "$work/base.img" "$work/barrier.img"
            "$work/probe" "$work/barrier.img" group-barrier "$fault" "$persist"
            "$work/probe" "$work/barrier.img" group-barrier-recovery
            "$work/probe" "$work/barrier.img" group-barrier-recovery
            e2fsck -fn "$work/barrier.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
        done
    done
    cp "$work/base.img" "$work/pipeline.img"
    "$work/probe" "$work/pipeline.img" group-ring
    e2fsck -fn "$work/pipeline.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    cp "$work/base.img" "$work/pipeline.img"
    "$work/probe" "$work/pipeline.img" group-pipeline
    e2fsck -fn "$work/pipeline.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    cp "$work/base.img" "$work/pipeline.img"
    "$work/probe" "$work/pipeline.img" group-commit-crash
    "$work/probe" "$work/pipeline.img" group-recovery
    e2fsck -fn "$work/pipeline.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    "$work/probe" "$work/disk.img" group
    e2fsck -fn "$work/disk.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    cp "$work/base.img" "$work/owner.img"
    "$work/probe" "$work/owner.img" group-data-owner
    e2fsck -fn "$work/owner.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    cp "$work/base.img" "$work/owner.img"
    "$work/probe" "$work/owner.img" group-data-batch
    e2fsck -fn "$work/owner.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    for mode in direct-data-batch direct-data-error direct-range-error; do
        truncate -s 32M "$work/direct.img"
        mkfs.ext4 -q -F -b "$block" -I 256 -O '^has_journal' "$work/direct.img"
        "$work/probe" "$work/direct.img" seed
        "$work/probe" "$work/direct.img" "$mode"
        e2fsck -fn "$work/direct.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    done
    for mode in group-partial-owner group-partial-error group-source-budget group-source-error; do
        cp "$work/base.img" "$work/owner.img"
        "$work/probe" "$work/owner.img" "$mode"
        "$work/probe" "$work/owner.img" group-recovery
        e2fsck -fn "$work/owner.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    done
    for fault in write flush; do
        cp "$work/base.img" "$work/owner.img"
        "$work/probe" "$work/owner.img" "group-data-$fault-error"
        "$work/probe" "$work/owner.img" group-recovery
        e2fsck -fn "$work/owner.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    done
    if [ "$block" = 4096 ]; then
        cp "$work/base.img" "$work/space.img"
        "$work/probe" "$work/space.img" group-space
        e2fsck -fn "$work/space.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
    fi
    for kind in group-write group-flush; do
        last=5
        [ "$kind" != group-flush ] || last=4
        for point in $(seq 1 "$last"); do
            cp "$work/base.img" "$work/disk.img"
            "$work/probe" "$work/disk.img" "$kind" "$point"
            "$work/probe" "$work/disk.img" group-recovery
            e2fsck -fn "$work/disk.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log" >&2; exit 1; }
        done
    done
    printf 'PASS: grouped versions and recovery block=%s\n' "$block"
done
