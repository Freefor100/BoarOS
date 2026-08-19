#!/bin/sh

set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
make_command=$(command -v make)
temporary_dir=$(mktemp -d)

trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

check_prefix()
{
	case_name=$1
	expected=$2
	shift 2
	case_bin="$temporary_dir/$case_name/bin"

	mkdir -p "$case_bin"
	for prefix in "$@"; do
		compiler="$case_bin/${prefix}gcc"
		: >"$compiler"
		chmod +x "$compiler"
	done

	actual=$(
		unset CROSS_COMPILE GNUMAKEFLAGS MAKEFILES MAKEFLAGS MFLAGS \
			MAKEOVERRIDES
		PATH="$case_bin" "$make_command" -s --no-print-directory \
			-C "$project_root" -f - print-cross-compile <<'EOF'
include Makefile

.PHONY: print-cross-compile
print-cross-compile:
	@prefix='$(CROSS_COMPILE)'; command -p printf '%s\n' "$$prefix"
EOF
	)

	if [ "$actual" != "$expected" ]; then
		printf '%s: expected %s, got %s\n' \
			"$case_name" "$expected" "$actual" >&2
		exit 1
	fi
}

check_prefix upstream riscv64-unknown-elf- riscv64-unknown-elf-
check_prefix arch riscv64-elf- riscv64-elf-
check_prefix priority riscv64-unknown-elf- \
	riscv64-unknown-elf- riscv64-elf-

printf '%s\n' 'toolchain prefix selection passed'
