#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${MM_FATAL_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-mm-fatal-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/mm-fatal.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing MM fatal test kernel: $kernel" >&2
    exit 1
fi

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$output" 2>&1; then
    tail -n 80 "$output" >&2
    echo "QEMU failed during MM fatal test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: MM resolution invariant test begin' "$output" || true)" -ne 1 ] ||
    [ "$(grep -cE '^BoarOS: fatal trap scause=0x3 sepc=0x[0-9a-f]+ stval=0x[0-9a-f]+ sstatus=0x[0-9a-f]+$' "$output" || true)" -ne 1 ]; then
    tail -n 80 "$output" >&2
    echo "page-table resolution failure did not produce one fatal trap" >&2
    exit 1
fi

if grep -qF 'BoarOS: MM resolution invariant escaped fatal' "$output"; then
    tail -n 80 "$output" >&2
    echo "page-table resolution failure returned to a retry path" >&2
    exit 1
fi

echo "RISC-V MM resolution invariant fatal passed"
