#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build_dir=${FILE_IO_HOST_BUILD:-build/host/ext4-acl-xattr}
: > "$build_dir/empty"
for shift in ${FILE_IO_HOST_SHIFTS:-12 14}; do
    image="$build_dir/nojournal-$shift.img"
    truncate -s 32M "$image"
    mkfs.ext4 -q -F -O ^has_journal -b "${FILE_IO_HOST_BLOCK_SIZE:-4096}" "$image"
    debugfs -w -R "write $build_dir/empty /acl-raw" "$image" > "$build_dir/nojournal-debugfs-$shift.log" 2>&1
    timeout 30s "$build_dir/concurrency-$shift" "$image"
    e2fsck -fn "$image" > "$build_dir/nojournal-fsck-$shift.log" 2>&1
done
