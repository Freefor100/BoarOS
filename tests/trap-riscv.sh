#!/bin/sh

set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/build/riscv/tests/kernel-trap-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/trap.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

show_output()
{
    tail -n 80 "$output" >&2
}

if [ ! -f "$kernel" ]; then
    echo "missing trap test kernel: $kernel" >&2
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
    echo "QEMU failed during trap test" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: trap test breakpoint=0x[0-9a-f]+$' "$output" || true)" -ne 1 ]; then
    show_output
    echo "expected one trap test breakpoint line" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: fatal trap scause=0x3 sepc=0x[0-9a-f]+ stval=0x[0-9a-f]+ sstatus=0x[0-9a-f]+$' "$output" || true)" -ne 1 ]; then
    show_output
    echo "expected one breakpoint trap diagnostic" >&2
    exit 1
fi

expected_sepc=$(sed -n 's/^BoarOS: trap test breakpoint=\(0x[0-9a-f]*\)$/\1/p' "$output")
actual_sepc=$(sed -n 's/^BoarOS: fatal trap .* sepc=\(0x[0-9a-f]*\) stval=.*/\1/p' "$output")

if [ "$actual_sepc" != "$expected_sepc" ]; then
    show_output
    echo "trap sepc mismatch: expected $expected_sepc, got $actual_sepc" >&2
    exit 1
fi

if grep -qF 'BoarOS: trap test returned' "$output"; then
    show_output
    echo "execution continued after the fatal trap" >&2
    exit 1
fi

echo "RISC-V trap test passed: breakpoint at $actual_sepc"
