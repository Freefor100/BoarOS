#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${UACCESS_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-uaccess-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/uaccess.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing uaccess test kernel: $kernel" >&2
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
    echo "QEMU failed during uaccess cases" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: uaccess cases result=0x0' "$output" || true)" -ne 1 ]; then
    tail -n 80 "$output" >&2
    echo "uaccess cases did not report success" >&2
    exit 1
fi

echo "RISC-V uaccess cases passed"
