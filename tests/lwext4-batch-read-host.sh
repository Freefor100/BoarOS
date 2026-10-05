#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-but-set-variable \
    -Wno-stringop-truncation -DCONFIG_USE_DEFAULT_CFG=1 ${BATCH_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_batch_read.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
python3 - "$work/data" <<'PY'
import sys
with open(sys.argv[1], 'wb') as f:
    for i in range(17):
        f.write(bytes([0 if 8 <= i < 12 else i+1]) * (123 if i==16 else 4096))
PY
for blocksize in 1024 4096 8192; do
    for extent in extent '^extent'; do
        truncate -s 128M "$work/root.img"
        mkfs.ext4 -q -F -b "$blocksize" -O "$extent,^64bit" "$work/root.img"
        debugfs -w -R "write $work/data /data" "$work/root.img" >/dev/null 2>&1
        timeout 30 "$work/probe" "$work/root.img"
    done
done
