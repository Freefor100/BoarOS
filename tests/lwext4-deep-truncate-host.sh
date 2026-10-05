#!/bin/sh
# A depth-two extent tree must remain checksummed after removing its leaves.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -Wno-unused-but-set-variable -Wno-stringop-truncation \
    -DCONFIG_USE_DEFAULT_CFG=1 ${TRUNCATE_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_truncate.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
for blocksize in 1024 4096; do
    truncate -s 64M "$work/root.img"
    mkfs.ext4 -q -F -b "$blocksize" -O orphan_file "$work/root.img"
    timeout 60 "$work/probe" "$work/root.img" deep-prepare 0 dense
    e2fsck -fn "$work/root.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    timeout 60 "$work/probe" "$work/root.img" deep-check 0 dense
    e2fsck -fn "$work/root.img" > "$work/fsck.log" 2>&1 || { cat "$work/fsck.log"; exit 1; }
    printf 'PASS: %s-byte depth-two extent truncate, restart read and final truncate\n' "$blocksize"
done
