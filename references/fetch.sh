#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
manifest="$script_dir/sources.tsv"
reference_root=$script_dir
active_temporary_path=

cleanup()
{
	if [ -n "$active_temporary_path" ]; then
		rm -rf -- "$active_temporary_path"
	fi
}

trap cleanup EXIT HUP INT TERM

usage()
{
	printf '%s\n' \
		"usage: $0 [--manifest PATH] [--root PATH]" >&2
	exit 2
}

while [ "$#" -gt 0 ]; do
	case $1 in
	--manifest)
		[ "$#" -ge 2 ] || usage
		manifest=$2
		shift 2
		;;
	--root)
		[ "$#" -ge 2 ] || usage
		reference_root=$2
		shift 2
		;;
	*)
		usage
		;;
	esac
done

fail()
{
	printf 'references: %s\n' "$*" >&2
	exit 1
}

require_command()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

validate_name()
{
	case $1 in
	''|/*|.|./*|*/.|*/./*|*//*|*/|..|../*|*/..|*/../*)
		fail "invalid destination: $1"
		;;
	esac
}

check_destination_path()
{
	relative_path=$1
	current_path=$reference_root

	while :; do
		case $relative_path in
		*/*)
			component=${relative_path%%/*}
			relative_path=${relative_path#*/}
			;;
		*)
			component=$relative_path
			relative_path=
			;;
		esac
		current_path="$current_path/$component"
		[ ! -L "$current_path" ] ||
			fail "$current_path is a symbolic link"
		[ -n "$relative_path" ] || break
	done
}

check_repository()
{
	destination=$1
	url=$2

	git -C "$destination" rev-parse --is-inside-work-tree \
		>/dev/null 2>&1 || fail "$destination is not a Git working tree"

	actual_url=$(git -C "$destination" remote get-url origin 2>/dev/null) ||
		fail "$destination has no origin remote"
	[ "$actual_url" = "$url" ] ||
		fail "$destination origin is $actual_url, expected $url"

	if [ -n "$(git -C "$destination" status --porcelain)" ]; then
		fail "$destination has local changes"
	fi
}

make_temporary_directory()
{
	name=$1
	mkdir -p -- "$reference_root"
	active_temporary_path=$(mktemp -d \
		"$reference_root/.${name}.tmp.XXXXXX")
}

restore_full_repository()
{
	name=$1
	url=$2
	expected_commit=$3
	destination="$reference_root/$name"

	require_command git
	if [ ! -e "$destination" ]; then
		make_temporary_directory "$name"
		git clone --quiet -- "$url" "$active_temporary_path"
		git -C "$active_temporary_path" cat-file -e \
			"$expected_commit^{commit}" 2>/dev/null ||
			fail "$name is missing commit $expected_commit"
		git -C "$active_temporary_path" checkout --quiet --detach \
			"$expected_commit"
		mv -- "$active_temporary_path" "$destination"
		active_temporary_path=
	fi

	check_repository "$destination" "$url"
	git -C "$destination" cat-file -e "$expected_commit^{commit}" \
		2>/dev/null || fail "$destination is missing commit $expected_commit"
	actual_commit=$(git -C "$destination" rev-parse HEAD)
	[ "$actual_commit" = "$expected_commit" ] ||
		fail "$destination is at $actual_commit, expected $expected_commit"
	printf 'references: ready %s (full repository)\n' "$name"
}

restore_snapshot_repository()
{
	name=$1
	url=$2
	remote_ref=$3
	expected_commit=$4
	destination="$reference_root/$name"

	require_command git
	if [ ! -e "$destination" ]; then
		make_temporary_directory "$name"
		git -C "$active_temporary_path" init --quiet
		git -C "$active_temporary_path" remote add origin "$url"
		git -C "$active_temporary_path" fetch --quiet --depth 1 \
			origin "$remote_ref"
		actual_commit=$(git -C "$active_temporary_path" \
			rev-parse 'FETCH_HEAD^{commit}')
		[ "$actual_commit" = "$expected_commit" ] ||
			fail "$name ref $remote_ref is $actual_commit, expected $expected_commit"
		git -C "$active_temporary_path" checkout --quiet --detach \
			"$expected_commit"
		mv -- "$active_temporary_path" "$destination"
		active_temporary_path=
	fi

	check_repository "$destination" "$url"
	actual_commit=$(git -C "$destination" rev-parse HEAD)
	[ "$actual_commit" = "$expected_commit" ] ||
		fail "$destination is at $actual_commit, expected $expected_commit"
	printf 'references: ready %s (%s)\n' "$name" "$expected_commit"
}

restore_file()
{
	name=$1
	url=$2
	expected_sha256=$3
	destination="$reference_root/$name"
	destination_parent=$(dirname -- "$destination")

	require_command curl
	require_command sha256sum
	mkdir -p -- "$destination_parent"

	if [ -e "$destination" ]; then
		actual_sha256=$(sha256sum "$destination" | awk '{ print $1 }')
		[ "$actual_sha256" = "$expected_sha256" ] ||
			fail "$destination checksum is $actual_sha256, expected $expected_sha256"
	else
		active_temporary_path=$(mktemp \
			"$destination_parent/.download.tmp.XXXXXX")
		curl --fail --location --silent --show-error \
			--retry 3 --output "$active_temporary_path" "$url"
		actual_sha256=$(sha256sum "$active_temporary_path" |
			awk '{ print $1 }')
		[ "$actual_sha256" = "$expected_sha256" ] ||
			fail "$name checksum is $actual_sha256, expected $expected_sha256"
		mv -- "$active_temporary_path" "$destination"
		active_temporary_path=
	fi

	printf 'references: ready %s\n' "$name"
}

[ -f "$manifest" ] || fail "missing manifest: $manifest"
mkdir -p -- "$reference_root"
reference_root=$(CDPATH='' cd -- "$reference_root" && pwd -P)

tab=$(printf '\t')
while IFS="$tab" read -r kind name url revision expected; do
	case $kind in
	''|'#'*)
		continue
		;;
	esac
	validate_name "$name"
	check_destination_path "$name"
	[ -n "$url" ] || fail "$name has no URL"
	[ -n "$expected" ] || fail "$name has no expected revision or checksum"

	case $kind in
	full)
		restore_full_repository "$name" "$url" "$expected"
		;;
	snapshot)
		[ -n "$revision" ] || fail "$name has no remote ref"
		restore_snapshot_repository \
			"$name" "$url" "$revision" "$expected"
		;;
	file)
		restore_file "$name" "$url" "$expected"
		;;
	*)
		fail "$name has unknown source kind: $kind"
		;;
	esac
done <"$manifest"
