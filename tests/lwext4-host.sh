#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work_dir=$(mktemp -d)
image="$work_dir/root.img"
uuid_seeded_image="$work_dir/uuid-seeded.img"
fixture="$work_dir/fixture"
expected='BoarOS lwext4 modern image probe'
host_cc=${HOST_CC:-cc}

trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

for tool in "$host_cc" truncate mkfs.ext4 debugfs dumpe2fs; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "missing lwext4 host-test tool: $tool" >&2
		exit 1
	fi
done

printf '%s' "$expected" >"$fixture"
truncate -s 128M "$image"
mkfs.ext4 -q -F "$image"

features=$(dumpe2fs -h "$image" 2>/dev/null |
	sed -n 's/^Filesystem features:[[:space:]]*//p')
case " $features " in
*' metadata_csum_seed '*)
	;;
*)
	mkfs.ext4 -q -F -O metadata_csum,metadata_csum_seed "$image"
	;;
esac

debugfs -w -R "write $fixture /boaros-probe" "$image" >/dev/null 2>&1
printf '%s\n' 'metadata_csum_seed image:'
dumpe2fs -h "$image" 2>/dev/null |
	sed -n '/^Filesystem features:/p;/^Block size:/p;/^Inode size:/p'

truncate -s 128M "$uuid_seeded_image"
mkfs.ext4 -q -F -O metadata_csum,^metadata_csum_seed "$uuid_seeded_image"
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
