#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${BLOCK_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-block-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
output="$output_dir/block.log"
disk="$output_dir/block.img"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

show_output()
{
    tail -n 100 "$output" >&2
}

if [ ! -f "$kernel" ]; then
    echo "missing VirtIO block test kernel: $kernel" >&2
    exit 1
fi

create_disks()
{
    mode=$1
    disk_rw="$output_dir/block-rw-$mode.img"
    disk_ro="$output_dir/block-ro-$mode.img"

    dd if=/dev/zero of="$disk_rw" bs=1M count=1 status=none
    printf '%s' 'BoarOS-direct-sector-one' |
        dd of="$disk_rw" bs=512 seek=1 conv=notrunc status=none
    printf '%s' 'BoarOS-bounce-window' |
        dd of="$disk_rw" bs=1 seek=1543 conv=notrunc status=none

    dd if=/dev/zero of="$disk_ro" bs=1M count=1 status=none
    printf '%s' 'BoarOS-direct-sector-one' |
        dd of="$disk_ro" bs=512 seek=1 conv=notrunc status=none
    printf '%s' 'BoarOS-bounce-window' |
        dd of="$disk_ro" bs=1 seek=1543 conv=notrunc status=none
}

run_case()
{
    mode=$1
    create_disks "$mode"
    output="$output_dir/block-$mode.log"
    set -- "$qemu" \
        -machine virt \
        -bios default \
        -kernel "$kernel" \
        -m 512M \
        -smp 1 \
        -nographic \
        -no-reboot
    if [ "$mode" = modern ]; then
        set -- "$@" -global virtio-mmio.force-legacy=false
    fi
    set -- "$@" \
        -drive file="$disk_rw",if=none,format=raw,readonly=off,id=x0 \
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
        -drive file="$disk_ro",if=none,format=raw,readonly=on,id=x1 \
        -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
    if ! timeout -k 2s 10s "$@" </dev/null >"$output" 2>&1; then
        show_output
        echo "QEMU failed during $mode VirtIO block test" >&2
        exit 1
    fi
    if [ "$(grep -cxF 'BoarOS: VirtIO block tests passed' "$output" || true)" -ne 1 ]; then
        show_output
        echo "$mode VirtIO block test kernel did not report success" >&2
        exit 1
    fi
    if grep -qF 'BoarOS: block test failed' "$output"; then
        show_output
        echo "$mode VirtIO block test kernel reported a failed case" >&2
        exit 1
    fi
}

run_case legacy
run_case modern

echo "RISC-V VirtIO block tests passed"
