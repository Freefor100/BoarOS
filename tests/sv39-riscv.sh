#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${SV39_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-sv39-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/sv39.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing Sv39 test kernel: $kernel" >&2
    exit 1
fi

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot \
    </dev/null >"$output" 2>&1; then
    tail -n 80 "$output" >&2
    echo "QEMU failed during Sv39 test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: Sv39 tests passed' "$output" || true)" -ne 1 ]; then
    tail -n 80 "$output" >&2
    echo "Sv39 test kernel did not report success" >&2
    exit 1
fi

if grep -qF 'BoarOS: Sv39 test failed' "$output"; then
    tail -n 80 "$output" >&2
    echo "Sv39 test kernel reported a failed case" >&2
    exit 1
fi

echo "RISC-V Sv39 tests passed"
