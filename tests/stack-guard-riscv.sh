#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${STACK_GUARD_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-stack-guard-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/stack-guard.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing stack guard test kernel: $kernel" >&2
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
    echo "QEMU failed during stack guard test" >&2
    exit 1
fi

line=$(sed -n \
    's/^BoarOS: stack guard low=\(0x[0-9a-f][0-9a-f]*\) guard=\(0x[0-9a-f][0-9a-f]*\)$/\1 \2/p' \
    "$output" | head -n 1)
low=${line%% *}
guard=${line##* }

case "$low" in
    0xffffffffc*)
        ;;
    *)
        tail -n 80 "$output" >&2
        echo "task stack was not mapped in the kernel stack window" >&2
        exit 1
        ;;
esac

if [ -z "$guard" ] ||
    ! grep -qxF 'BoarOS: stack guard mapped write ok' "$output" ||
    ! grep -Eq "^BoarOS: fatal trap scause=0xf sepc=0x[0-9a-f]+ stval=$guard sstatus=0x[0-9a-f]+$" "$output"; then
    tail -n 80 "$output" >&2
    echo "stack guard page did not produce the expected store fault" >&2
    exit 1
fi

if grep -qF 'BoarOS: stack guard write returned' "$output" ||
    grep -qF 'BoarOS: stack guard not enforced' "$output" ||
    grep -qF 'BoarOS: stack guard layout mismatch' "$output" ||
    grep -qF 'BoarOS: stack guard setup failed' "$output"; then
    tail -n 80 "$output" >&2
    echo "stack guard test reached an invalid path" >&2
    exit 1
fi

echo "RISC-V kernel stack guard page passed"
