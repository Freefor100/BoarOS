#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'r=$?; if [ "$r" -eq 0 ]; then rm -rf "$work"; else echo "artifacts: $work" >&2; fi' EXIT
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-but-set-variable -Wno-stringop-truncation \
 -DBOAROS_MAX_CPUS=2 -DCONFIG_USE_DEFAULT_CFG=1 -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
 "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_tmpfile.c" "$root/tests/host/block_fault.c" \
 "$root/kernel/block.c" "$root/tests/host/block_runtime.c" "$root/kernel/raw_lock.c" \
 "$root/tests/host/sync/notifications.c" -I"$root/tests/host/block_model" -pthread -o "$work/probe"
for format in orphan_file '^orphan_file'; do
 truncate -s 32M "$work/disk.img"
 mkfs.ext4 -q -F -b 1024 -O "$format" "$work/disk.img"
 timeout 30 "$work/probe" "$work/disk.img" basic
 e2fsck -fn "$work/disk.img" >/dev/null
 timeout 30 "$work/probe" "$work/disk.img" crash
 timeout 30 "$work/probe" "$work/disk.img" verify
 e2fsck -fn "$work/disk.img" >/dev/null
done
