#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
memory=${QEMU_MEMORY:-512M}
output_dir=$(mktemp -d)
output="$output_dir/idle.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing production kernel: $kernel" >&2
    exit 1
fi

set +e
timeout -k 2s 2s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m "$memory" \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$output" 2>&1
status=$?
set -e

if [ "$status" -ne 124 ]; then
    tail -n 100 "$output" >&2
    echo "production kernel did not remain in wfi (status=$status)" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: timer frequency=0x989680 tick-hz=0x64 period=0x186a0' "$output" || true)" -ne 1 ]; then
    tail -n 100 "$output" >&2
    echo "production kernel did not initialize the timer" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: physical allocator mode=buddy metadata=0x[1-9a-f][0-9a-f]*$' "$output" || true)" -ne 1 ]; then
    tail -n 100 "$output" >&2
    echo "production kernel did not finalize the buddy allocator" >&2
    exit 1
fi

if grep -qE 'BoarOS: (fatal trap|timer error|SBI shutdown failed)' "$output"; then
    tail -n 100 "$output" >&2
    echo "production kernel reported an error while idle" >&2
    exit 1
fi

echo "RISC-V production kernel remained idle with timer interrupts enabled"
