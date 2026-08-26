#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${USER_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-user-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/user.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing user-mode test kernel: $kernel" >&2
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
    echo "QEMU failed during user-mode test" >&2
    exit 1
fi

if [ "$(grep -cxE 'BoarOS: user mode completions=0x4 ticks=0x[1-9a-f][0-9a-f]* failures=0x0' "$output" || true)" -ne 1 ]; then
    tail -n 100 "$output" >&2
    echo "user-mode test did not complete the preempt/resume/exit path" >&2
    exit 1
fi

echo "RISC-V user-mode preemption and exit passed"
