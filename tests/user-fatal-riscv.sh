#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${USER_FATAL_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-user-fatal-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/user-fatal.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing user-mode fatal test kernel: $kernel" >&2
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
    tail -n 100 "$output" >&2
    echo "QEMU failed during user-mode fatal test" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: invalid trap return sstatus=0x[0-9a-f]+ sepc=0x[0-9a-f]+$' "$output" || true)" -ne 1 ]; then
    tail -n 100 "$output" >&2
    echo "bad return under a user root did not reach fatal shutdown" >&2
    exit 1
fi

echo "RISC-V user-root fatal diagnostic passed"
