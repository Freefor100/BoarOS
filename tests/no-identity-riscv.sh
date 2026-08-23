#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${NO_IDENTITY_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-no-identity-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/no-identity.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

show_output()
{
    tail -n 80 "$output" >&2
}

if [ ! -f "$kernel" ]; then
    echo "missing no-identity test kernel: $kernel" >&2
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
    echo "QEMU failed during no-identity test" >&2
    exit 1
fi

target=$(sed -n \
    's/^BoarOS: no-identity target=\(0x[0-9a-f][0-9a-f]*\)$/\1/p' \
    "$output")

if [ "$target" != "0x80200000" ] ||
    ! grep -Eq '^BoarOS: direct map pa=0x[0-9a-f]+ va=0xffffffc[0-9a-f]+ .* value=0x1122334455667788 reused=0x[0-9a-f]+$' \
        "$output" ||
    ! grep -Eq '^BoarOS: high-half pc=0xffffffff[89a-f][0-9a-f]+ .*' \
        "$output" ||
    ! grep -Eq "^BoarOS: fatal trap scause=0xd sepc=0xffffffff[89a-f][0-9a-f]+ stval=$target sstatus=0x[0-9a-f]+$" \
        "$output"; then
    show_output
    echo "low kernel RAM alias did not produce the expected load page fault" >&2
    exit 1
fi

if grep -qF 'BoarOS: no-identity load returned' "$output"; then
    show_output
    echo "low kernel RAM alias remained readable after final satp switch" >&2
    exit 1
fi

echo "RISC-V final address space has no low RAM identity alias"
