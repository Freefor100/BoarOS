#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${SCHEDULER_BOOT_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-scheduler-boot-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/scheduler.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing scheduler boot kernel: $kernel" >&2
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
    tail -n 120 "$output" >&2
    echo "QEMU failed during scheduler preemption test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: scheduler preemption order=0x1212 workers=0x3 reaped=0x2 failures=0x0' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "scheduler did not complete timer-only A-B-A preemption" >&2
    exit 1
fi
if grep -qE 'BoarOS: (fatal trap|timer error|scheduler error|scheduler preemption timeout)' \
    "$output"; then
    tail -n 120 "$output" >&2
    echo "scheduler preemption test reported a fatal path" >&2
    exit 1
fi

echo "RISC-V timer-only scheduler preemption passed"
