#!/bin/sh

set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$root/kernel-rv"}
program=${ELF_RWX_PROGRAM_RV:-"$root/build/riscv/tests/user/elf-rwx-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
work_dir=$(mktemp -d "$root/build/riscv/elf-rwx-run.XXXXXX")
disk="$work_dir/root.img"
output="$work_dir/output.log"

trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work_dir"; else echo "ELF RWX artifacts retained: $work_dir" >&2; fi' EXIT

if ! readelf -lW "$program" | awk '$1 == "LOAD" && $7 == "RWE" { found = 1 } END { exit !found }'; then
    echo 'ELF RWX fixture lacks a writable executable PT_LOAD' >&2
    exit 1
fi

truncate -s 16M "$disk"
mkfs.ext4 -q -F "$disk"
debugfs -w -R "write $program /init" "$disk" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /init mode 0100755' "$disk" >/dev/null 2>&1

if ! timeout -k 2s 30s "$qemu" -machine virt -bios default \
    -kernel "$kernel" -m 512M -smp 1 -nographic -no-reboot \
    -drive file="$disk",if=none,format=raw,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    >"$output" 2>&1; then
    tail -n 30 "$output" >&2
    exit 1
fi
if [ "$(grep -cxF 'ELF RWX PASS' "$output" || true)" -ne 1 ] ||
    ! grep -q 'BoarOS: PID 1 exited status=0x0' "$output" ||
    grep -qE 'BoarOS: (fatal trap|root boot error)' "$output"; then
    tail -n 30 "$output" >&2
    exit 1
fi

echo 'ELF writable executable segment passed'
