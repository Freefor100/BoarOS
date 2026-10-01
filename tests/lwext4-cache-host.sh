#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
    -DCONFIG_DEBUG_ASSERT=0 -DCONFIG_DEBUG_PRINTF=0 \
    -include "$root/tests/host/lwext4_memory.h" \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root/third_party/lwext4/src/ext4_bcache.c" \
    "$root/third_party/lwext4/src/ext4_blockdev.c" \
    "$root/tests/host/lwext4_cache.c" -o "$work/probe"
"$work/probe"
