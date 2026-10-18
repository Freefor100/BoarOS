#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build=${LOOP_HOST_BUILD:-build/host/loop}
mkdir -p "$build"
for block in ${LOOP_HOST_BLOCKS:-1024 4096}; do
    outer="$build/outer-$block.img"; inner="$build/inner-$block.img"
    truncate -s 16M "$outer"; truncate -s 8M "$inner"
    mkfs.ext4 -q -F -b "$block" "$outer"
    mkfs.ext4 -q -F -b "$block" "$inner"
    for shift in ${LOOP_HOST_SHIFTS:-12 14}; do
        result="$build/stack-$shift-$block.img"
        BOAROS_LOOP_OUTER_IMAGE="$root/$outer" BOAROS_LOOP_INNER_IMAGE="$root/$inner" \
        BOAROS_LOOP_OUTER_RESULT="$root/$result" FILE_IO_HOST_BUILD="$build" \
        FILE_IO_HOST_TCP=0 FILE_IO_HOST_SHIFTS="$shift" FILE_IO_HOST_FLAGS=-DLOOP_IO_CASES \
            sh tests/file-io-concurrency-host.sh
        e2fsck -fn "$result" > "$build/fsck-stack-$shift-$block.log" 2>&1
        debugfs -R "dump /inner-backing $build/inner-result-$shift-$block.img" "$result" > "$build/dump-$shift-$block.log" 2>&1
        e2fsck -fn "$build/inner-result-$shift-$block.img" > "$build/fsck-inner-$shift-$block.log" 2>&1
        debugfs -R 'cat /payload' "$build/inner-result-$shift-$block.img" 2>/dev/null > "$build/payload-$shift-$block"
        [ "$(cat "$build/payload-$shift-$block")" = 'nested-loop-durable' ]
    done
done
