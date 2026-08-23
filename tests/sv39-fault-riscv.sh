#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${SV39_FAULT_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-sv39-fault-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/sv39-fault.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing Sv39 fault test kernel: $kernel" >&2
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
    echo "QEMU failed during Sv39 permission test" >&2
    exit 1
fi

target=$(sed -n \
    's/^BoarOS: Sv39 readonly target=\(0x[0-9a-f][0-9a-f]*\)$/\1/p' \
    "$output")
if [ -z "$target" ] ||
    ! grep -qxF 'BoarOS: Sv39 readonly read=0x1122334455667788' \
        "$output" ||
    ! grep -Eq "^BoarOS: fatal trap scause=0xf sepc=0x[0-9a-f]+ stval=$target sstatus=0x[0-9a-f]+$" "$output"; then
    tail -n 80 "$output" >&2
    echo "Sv39 readonly page did not produce the expected store fault" >&2
    exit 1
fi

if grep -qF 'BoarOS: Sv39 permission setup failed' "$output" ||
    grep -qF 'BoarOS: Sv39 readonly write returned' "$output"; then
    tail -n 80 "$output" >&2
    echo "Sv39 permission test reached an invalid path" >&2
    exit 1
fi

echo "RISC-V Sv39 permission fault passed"
