#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${FILES_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-files-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
work_dir=$(mktemp -d)
output="$work_dir/files.log"
disk="$work_dir/root.img"
fixture="$work_dir/data"

trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

for tool in truncate mkfs.ext4 debugfs awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing files test tool: $tool" >&2
        exit 1
    fi
done

if [ ! -f "$kernel" ]; then
    echo "missing files test kernel: $kernel" >&2
    exit 1
fi

awk 'BEGIN { for (i = 0; i < 9000; i++) printf "%c", 65 + (i % 26) }' \
    >"$fixture"
truncate -s 32M "$disk"
mkfs.ext4 -q -F "$disk"
debugfs -w -R "write $fixture /data" "$disk" >/dev/null 2>&1

if ! timeout -k 2s 20s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot \
    -global virtio-mmio.force-legacy=false \
    -drive file="$disk",if=none,format=raw,readonly=on,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    </dev/null >"$output" 2>&1; then
    tail -n 120 "$output" >&2
    echo "QEMU failed during process files test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: process files tests passed' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "process files test kernel did not report success" >&2
    exit 1
fi

if grep -qF 'BoarOS: process files test failed' "$output"; then
    tail -n 120 "$output" >&2
    echo "process files test kernel reported a failed case" >&2
    exit 1
fi

echo "RISC-V process files tests passed"
