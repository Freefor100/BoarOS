#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "test artifacts retained: $work" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
cc=${HOST_CC:-cc}
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-but-set-variable \
    -Wno-stringop-truncation -DCONFIG_USE_DEFAULT_CFG=1 ${ORPHAN_TEST_CFLAGS:-} \
    -I"$root/third_party/lwext4/include" -idirafter "$root/include" \
    "$root"/third_party/lwext4/src/*.c "$root/tests/host/lwext4_orphan.c" \
    "$root/tests/host/block_fault.c" "$root/kernel/block.c" -o "$work/probe"
printf 'persistent orphan fixture' > "$work/data"
for features in '^orphan_file' orphan_file 'orphan_file,^metadata_csum_seed' 'orphan_file,^metadata_csum,^metadata_csum_seed'; do
    image="$work/$features.img"
    truncate -s 32M "$image"
    mkfs.ext4 -q -F -b 1024 -O "$features" "$image"
    for name in a b c; do
        debugfs -w -R "write $work/data /$name" "$image" >/dev/null 2>&1
    done
    cp "$image" "$work/base-$features.img"
    timeout 30 "$work/probe" "$image" readonly
    timeout 30 "$work/probe" "$image" add
    cp "$image" "$work/pending-$features.img"
    timeout 30 "$work/probe" "$image" remove-one
    timeout 30 "$work/probe" "$image" remove-one
    timeout 30 "$work/probe" "$image" finish
    e2fsck -fn "$image" >/dev/null
    printf 'PASS: %s add/remove, crash enumeration, repeated recovery\n' "$features"
done

for damage in file-magic file-checksum file-range file-free file-duplicate \
              chain-cycle chain-range chain-free mixed-duplicate \
              bitmap-checksum bitmap-uninit; do
    case "$damage" in file-*|mixed-duplicate) source="$work/pending-orphan_file.img" ;; *) source="$work/pending-^orphan_file.img" ;; esac
    cp "$source" "$work/damaged.img"
    timeout 30 "$work/probe" "$work/damaged.img" "damage-$damage"
    errno=117
    [ "$damage" != file-checksum ] || errno=5
    timeout 30 "$work/probe" "$work/damaged.img" "reject-$errno"
done
printf '%s\n' 'PASS: invalid orphan records preserve roots, cycles, allocation, magic/checksum'

# Shrink the preallocated orphan file to one valid block so its actual capacity
# can be filled with distinct allocated inodes, then exercise Linux's fallback.
cp "$work/base-orphan_file.img" "$work/full.img"
orphan_ino=$(dumpe2fs -h "$work/full.img" 2>/dev/null | sed -n 's/^Orphan file inode:[[:space:]]*//p')
debugfs -w -R "punch <$orphan_ino> 1 4294967295" "$work/full.img" >/dev/null 2>&1
debugfs -w -R "set_inode_field <$orphan_ino> size 1024" "$work/full.img" >/dev/null 2>&1
i=0
while [ "$i" -le 254 ]; do
    printf 'write %s /fill-%s\n' "$work/data" "$i" >> "$work/fill.commands"
    i=$((i + 1))
done
debugfs -w -f "$work/fill.commands" "$work/full.img" >/dev/null 2>&1
e2fsck -fn "$work/full.img" >/dev/null
timeout 60 "$work/probe" "$work/full.img" fill
timeout 60 "$work/probe" "$work/full.img" drain
e2fsck -fn "$work/full.img" >/dev/null
printf '%s\n' 'PASS: full orphan_file falls back to legacy list, both drain after crash'

for format in '^orphan_file' orphan_file; do
    cp "$work/base-$format.img" "$work/error.img"
    timeout 30 "$work/probe" "$work/error.img" add-error
    timeout 30 "$work/probe" "$work/error.img" finish
    e2fsck -fn "$work/error.img" >/dev/null
    cp "$work/pending-$format.img" "$work/error.img"
    timeout 30 "$work/probe" "$work/error.img" remove-error
    timeout 30 "$work/probe" "$work/error.img" remove-one
    timeout 30 "$work/probe" "$work/error.img" remove-one
    timeout 30 "$work/probe" "$work/error.img" finish
    e2fsck -fn "$work/error.img" >/dev/null
    cp "$work/pending-$format.img" "$work/independent.img"
    e2fsck -fy "$work/independent.img" > "$work/fsck.log" 2>&1 || {
        status=$?
        [ "$status" -le 1 ] || { cat "$work/fsck.log"; exit "$status"; }
    }
    timeout 30 "$work/probe" "$work/independent.img" finish
    e2fsck -fn "$work/independent.img" >/dev/null
done
printf '%s\n' 'PASS: failed add/remove commit retains crash state, independent e2fsck cleanup'
