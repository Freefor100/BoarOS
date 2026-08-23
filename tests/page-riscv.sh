#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${PAGE_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-page-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/page.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

show_output()
{
    tail -n 80 "$output" >&2
}

if [ ! -f "$kernel" ]; then
    echo "missing physical page test kernel: $kernel" >&2
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
    show_output
    echo "QEMU failed during physical page test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: physical page tests passed' "$output" || true)" -ne 1 ]; then
    show_output
    echo "physical page test kernel did not report success" >&2
    exit 1
fi

if grep -qF 'BoarOS: physical page test failed' "$output"; then
    show_output
    echo "physical page test kernel reported a failed case" >&2
    exit 1
fi

echo "RISC-V physical page tests passed"
