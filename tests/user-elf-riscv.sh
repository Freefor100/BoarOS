#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${USER_ELF_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-user-elf-rv"}
program=${USER_ELF_PROGRAM_RV:-"$project_root/build/riscv/tests/user/elf-probe-rv"}
fault_program=${USER_ELF_FAULT_PROGRAM_RV:-"$project_root/build/riscv/tests/user/elf-text-fault-rv"}
readelf_rv=${READELF_RV:-riscv64-unknown-elf-readelf}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/user-elf.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

for file in "$kernel" "$program" "$fault_program"; do
    if [ ! -f "$file" ]; then
        echo "missing user ELF test artifact: $file" >&2
        exit 1
    fi
done

for file in "$program" "$fault_program"; do
    header=$($readelf_rv -hW "$file")
    segments=$($readelf_rv -lW "$file")
    sections=$($readelf_rv -SW "$file")

    if ! printf '%s\n' "$header" | grep -qE 'Type:[[:space:]]+EXEC' ||
       ! printf '%s\n' "$header" | grep -qE 'Machine:[[:space:]]+RISC-V' ||
       [ "$(printf '%s\n' "$segments" | grep -cE '^[[:space:]]+LOAD' || true)" -ne 2 ] ||
       printf '%s\n' "$segments" | grep -qE '^[[:space:]]+INTERP' ||
       ! printf '%s\n' "$segments" | grep -qE 'LOAD.*R E.*0x1000' ||
       ! printf '%s\n' "$segments" | grep -qE 'LOAD.*RW .*0x1000' ||
       ! printf '%s\n' "$sections" | grep -qE '[[:space:]]\.bss[[:space:]]+NOBITS[[:space:]]'; then
        echo "unexpected standalone user ELF layout: $file" >&2
        exit 1
    fi
done

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$output" 2>&1; then
    tail -n 100 "$output" >&2
    echo "QEMU failed during user ELF execution" >&2
    exit 1
fi

if [ "$(grep -cxE 'BoarOS: user ELF completions=0x2 ticks=0x[1-9a-f][0-9a-f]* failures=0x0' "$output" || true)" -ne 1 ]; then
    tail -n 100 "$output" >&2
    echo "standalone user ELF programs did not complete correctly" >&2
    exit 1
fi

echo "RISC-V standalone user ELF execution passed"
