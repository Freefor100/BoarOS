#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${FILES_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-files-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
work_dir=$(mktemp -d)
output="$work_dir/files.log"
disk="$work_dir/root.img"
fixture="$work_dir/data"
small_fixture="$work_dir/allocated"

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
printf 'x' >"$small_fixture"
truncate -s 32M "$disk"
mkfs.ext4 -q -F "$disk"
debugfs -w -R "write $fixture /data" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data mode 0100640" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data uid 1234" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data gid 2345" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data atime @1700000001" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data atime_extra 444" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data mtime @1700000002" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data mtime_extra 888" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data ctime @1700000003" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /data ctime_extra 1332" "$disk" >/dev/null 2>&1
debugfs -w -R "write $small_fixture /allocated" "$disk" >/dev/null 2>&1
long_name=$(awk 'BEGIN { for (i = 0; i < 255; i++) printf "n" }')
debugfs -w -R "write $fixture /$long_name" "$disk" >/dev/null 2>&1

if ! timeout -k 2s 20s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot \
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

if [ "$(grep -cxF 'BoarOS: files console write ok' "$output" || true)" -lt 2 ]; then
    tail -n 120 "$output" >&2
    echo "process files test kernel did not write through the console descriptor" >&2
    exit 1
fi

if grep -qF 'BoarOS: process files test failed' "$output"; then
    tail -n 120 "$output" >&2
    echo "process files test kernel reported a failed case" >&2
    exit 1
fi

echo "RISC-V process files tests passed"
