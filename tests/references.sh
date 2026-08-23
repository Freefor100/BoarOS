#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
fetch_script="$project_root/references/fetch.sh"
temporary_dir=$(mktemp -d)

trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

source_work="$temporary_dir/source-work"
source_bare="$temporary_dir/source.git"
reference_root="$temporary_dir/references"
manifest="$temporary_dir/sources.tsv"
payload="$temporary_dir/manual.pdf"

expect_failure()
{
	expected_message=$1
	output_name=$2
	shift 2
	if "$@" >"$temporary_dir/$output_name.out" \
		2>"$temporary_dir/$output_name.err"; then
		printf 'unexpected success: %s\n' "$output_name" >&2
		exit 1
	fi
	if ! grep -qF "$expected_message" \
		"$temporary_dir/$output_name.err"; then
		cat "$temporary_dir/$output_name.err" >&2
		printf 'failure was not precise: %s\n' "$output_name" >&2
		exit 1
	fi
}

git init -q -b main "$source_work"
git -C "$source_work" config user.name 'BoarOS test'
git -C "$source_work" config user.email 'boaros-test@example.invalid'
printf '%s\n' initial >"$source_work/history.txt"
git -C "$source_work" add history.txt
git -C "$source_work" commit -q -m initial
initial_commit=$(git -C "$source_work" rev-parse HEAD)

git -C "$source_work" switch -q -c feature
printf '%s\n' feature >"$source_work/feature.txt"
git -C "$source_work" add feature.txt
git -C "$source_work" commit -q -m feature
feature_commit=$(git -C "$source_work" rev-parse HEAD)
git -C "$source_work" tag -a -m reference reference-tag

git -C "$source_work" switch -q main
printf '%s\n' current >>"$source_work/history.txt"
git -C "$source_work" commit -q -am current
main_commit=$(git -C "$source_work" rev-parse HEAD)
git clone -q --bare "$source_work" "$source_bare"

printf '%s\n' 'manual payload' >"$payload"
payload_sha=$(sha256sum "$payload" | awk '{ print $1 }')

tab=$(printf '\t')
{
	printf 'full%ssuite%sfile://%s%s-%s%s\n' \
		"$tab" "$tab" "$source_bare" "$tab" "$tab" "$main_commit"
	printf 'snapshot%ssnapshot%sfile://%s%srefs/heads/feature%s%s\n' \
		"$tab" "$tab" "$source_bare" "$tab" "$tab" "$feature_commit"
	printf 'snapshot%stag-snapshot%sfile://%s%srefs/tags/reference-tag%s%s\n' \
		"$tab" "$tab" "$source_bare" "$tab" "$tab" "$feature_commit"
	printf 'file%smanual.pdf%sfile://%s%s-%s%s\n' \
		"$tab" "$tab" "$payload" "$tab" "$tab" "$payload_sha"
} >"$manifest"

"$fetch_script" --manifest "$manifest" --root "$reference_root"

test "$(git -C "$reference_root/suite" rev-parse HEAD)" = "$main_commit"
test "$(git -C "$reference_root/suite" symbolic-ref -q HEAD || true)" = ''
git -C "$reference_root/suite" cat-file -e "$initial_commit^{commit}"
git -C "$reference_root/suite" show-ref --verify --quiet \
	refs/remotes/origin/feature
git -C "$reference_root/suite" show-ref --verify --quiet \
	refs/tags/reference-tag
test "$(git -C "$reference_root/snapshot" rev-parse HEAD)" = \
	"$feature_commit"
test "$(git -C "$reference_root/snapshot" symbolic-ref -q HEAD || true)" = ''
test "$(git -C "$reference_root/tag-snapshot" rev-parse HEAD)" = \
	"$feature_commit"
test "$(sha256sum "$reference_root/manual.pdf" | awk '{ print $1 }')" = \
	"$payload_sha"

"$fetch_script" --manifest "$manifest" --root "$reference_root"

git -C "$reference_root/snapshot" switch -q -c local-analysis
"$fetch_script" --manifest "$manifest" --root "$reference_root"

git -C "$reference_root/suite" checkout -q --detach "$feature_commit"
expect_failure "is at $feature_commit, expected $main_commit" \
	wrong-full-commit "$fetch_script" --manifest "$manifest" \
	--root "$reference_root"
git -C "$reference_root/suite" checkout -q main

git -C "$reference_root/snapshot" remote set-url origin invalid://origin
expect_failure 'origin is invalid://origin' wrong-origin \
	"$fetch_script" --manifest "$manifest" --root "$reference_root"
git -C "$reference_root/snapshot" remote set-url origin \
	"file://$source_bare"

git -C "$reference_root/snapshot" fetch -q --depth 1 origin \
	refs/heads/main
git -C "$reference_root/snapshot" checkout -q --detach "$main_commit"
expect_failure "is at $main_commit, expected $feature_commit" \
	wrong-snapshot-commit "$fetch_script" --manifest "$manifest" \
	--root "$reference_root"
git -C "$reference_root/snapshot" checkout -q --detach "$feature_commit"

printf '%s\n' damaged >"$reference_root/manual.pdf"
expect_failure 'manual.pdf checksum is' wrong-file-hash \
	"$fetch_script" --manifest "$manifest" --root "$reference_root"
cp "$payload" "$reference_root/manual.pdf"

escape_root="$temporary_dir/escape"
mkdir "$escape_root"
ln -s "$escape_root" "$reference_root/nested"
symlink_manifest="$temporary_dir/symlink-sources.tsv"
printf 'file%snested/escaped.pdf%sfile://%s%s-%s%s\n' \
	"$tab" "$tab" "$payload" "$tab" "$tab" "$payload_sha" \
	>"$symlink_manifest"
expect_failure 'is a symbolic link' symlink-parent \
	"$fetch_script" --manifest "$symlink_manifest" \
	--root "$reference_root"
test ! -e "$escape_root/escaped.pdf"

printf '%s\n' dirty >>"$reference_root/suite/history.txt"
expect_failure 'has local changes' dirty-repository \
	"$fetch_script" --manifest "$manifest" --root "$reference_root"

printf '%s\n' 'reference restoration passed'
