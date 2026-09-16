#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work_dir=$(mktemp -d)
image="$work_dir/root.img"
uuid_seeded_image="$work_dir/uuid-seeded.img"
sparse_image="$work_dir/sparse.img"
legacy_image="$work_dir/legacy.img"
wide_legacy_image="$work_dir/wide-legacy.img"
fixture="$work_dir/fixture"
empty_fixture="$work_dir/empty"
expected='BoarOS lwext4 modern image probe'
host_cc=${HOST_CC:-cc}

trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

for tool in "$host_cc" truncate mkfs.ext4 debugfs dumpe2fs e2fsck dd; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "missing lwext4 host-test tool: $tool" >&2
		exit 1
	fi
done

printf '%s' "$expected" >"$fixture"
printf '' >"$empty_fixture"
truncate -s 128M "$image"
mkfs.ext4 -q -F -b 1024 -I 256 "$image"

features=$(dumpe2fs -h "$image" 2>/dev/null |
	sed -n 's/^Filesystem features:[[:space:]]*//p')
case " $features " in
*' metadata_csum_seed '*)
	;;
*)
	mkfs.ext4 -q -F -b 1024 -I 256 \
		-O metadata_csum,metadata_csum_seed "$image"
	;;
esac

debugfs -w -R "write $fixture /boaros-probe" "$image" >/dev/null 2>&1
printf '%s\n' 'metadata_csum_seed image:'
dumpe2fs -h "$image" 2>/dev/null |
	sed -n '/^Filesystem features:/p;/^Block size:/p;/^Inode size:/p'

truncate -s 128M "$uuid_seeded_image"
mkfs.ext4 -q -F -b 1024 -I 256 \
	-O metadata_csum,^metadata_csum_seed "$uuid_seeded_image"
debugfs -w -R "write $fixture /boaros-probe" "$uuid_seeded_image" \
	>/dev/null 2>&1
uuid_seeded_features=$(dumpe2fs -h "$uuid_seeded_image" 2>/dev/null |
	sed -n 's/^Filesystem features:[[:space:]]*//p')
case " $uuid_seeded_features " in
*' metadata_csum_seed '*)
	echo 'failed to create UUID-seeded ext4 test image' >&2
	exit 1
	;;
esac
printf '%s\n' 'UUID-derived checksum seed image:'
dumpe2fs -h "$uuid_seeded_image" 2>/dev/null |
	sed -n '/^Filesystem features:/p;/^Block size:/p;/^Inode size:/p'

mkdir "$work_dir/objects"
objects=
for source in "$project_root"/third_party/lwext4/src/*.c; do
	object="$work_dir/objects/$(basename "${source%.c}").o"
	"$host_cc" -std=gnu11 -O2 -Wall -Wextra -Werror \
		-Wno-unused-but-set-variable -Wno-stringop-truncation \
		-DCONFIG_USE_DEFAULT_CFG=1 \
		-I"$project_root/third_party/lwext4/include" \
		-c "$source" -o "$object"
	objects="$objects $object"
done

"$host_cc" -std=gnu11 -O2 -Wall -Wextra -Werror \
	-DCONFIG_USE_DEFAULT_CFG=1 \
	-I"$project_root/third_party/lwext4/include" \
	-c "$project_root/tests/host/lwext4_read.c" \
	-o "$work_dir/objects/lwext4_read.o"

# objects is assembled only from the fixed source directory above; splitting is
# intentional so warnings in the BoarOS harness and vendored source are checked
# with their respective flags.
# shellcheck disable=SC2086
"$host_cc" $objects "$work_dir/objects/lwext4_read.o" \
	-o "$work_dir/lwext4-read"

"$work_dir/lwext4-read" "$image" /boaros-probe "$expected"
"$work_dir/lwext4-read" "$uuid_seeded_image" /boaros-probe "$expected"

# Timestamp encoding is an on-disk contract for both old and extended inodes.
for inode_size in 128 256; do
    time_image="$work_dir/timestamps-$inode_size.img"
    truncate -s 32M "$time_image"
    mkfs.ext4 -q -F -b 1024 -I "$inode_size" "$time_image"
    debugfs -w -R "write $fixture /boaros-probe" "$time_image" >/dev/null 2>&1
    "$work_dir/lwext4-read" "$time_image" --timestamps
    "$work_dir/lwext4-read" "$time_image" --timestamps-readonly
    e2fsck -fn "$time_image" >/dev/null
 done

# Preserve both checksum images as read-only probes. Sparse mutations happen
# only in this dedicated copy, whose block size is fixed for exact hole tests.
cp "$image" "$sparse_image"
printf '%s' 'physical-block-zero-must-not-leak' |
	dd of="$sparse_image" bs=1 conv=notrunc status=none
debugfs -w -R "write $empty_fixture /aligned-hole" "$sparse_image" \
	>/dev/null 2>&1
debugfs -w -R "set_inode_field /aligned-hole size 1024" "$sparse_image" \
	>/dev/null 2>&1
debugfs -w -R "write $empty_fixture /unwritten-partial" "$sparse_image" \
	>/dev/null 2>&1
debugfs -w -R "fallocate /unwritten-partial 0 0" "$sparse_image" \
	>/dev/null 2>&1
debugfs -w -R "set_inode_field /unwritten-partial size 1024" "$sparse_image" \
	>/dev/null 2>&1

truncate -s 128M "$legacy_image"
mkfs.ext4 -q -F -b 1024 -I 256 -O ^extent,^64bit "$legacy_image"
debugfs -w -R "write $empty_fixture /legacy-limit" "$legacy_image" \
	>/dev/null 2>&1

truncate -s 128M "$wide_legacy_image"
mkfs.ext4 -q -F -b 8192 -I 256 -O ^extent,^64bit "$wide_legacy_image"
debugfs -w -R "write $empty_fixture /wide-legacy-limit" \
	"$wide_legacy_image" >/dev/null 2>&1

sparse_status=0
"$work_dir/lwext4-read" "$sparse_image" --aligned-hole || sparse_status=1
"$work_dir/lwext4-read" "$sparse_image" --sparse || sparse_status=1
"$work_dir/lwext4-read" "$legacy_image" --legacy-limit || sparse_status=1
"$work_dir/lwext4-read" "$wide_legacy_image" --wide-legacy-limit || \
	sparse_status=1
e2fsck -fn "$sparse_image" || sparse_status=1
e2fsck -fn "$legacy_image" || sparse_status=1
e2fsck -fn "$wide_legacy_image" || sparse_status=1
exit "$sparse_status"
