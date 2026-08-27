#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
init=${ROOT_INIT_PROGRAM_RV:-"$project_root/build/riscv/tests/user/root-init-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
memory=${QEMU_MEMORY:-512M}
work_dir=$(mktemp -d)
output="$work_dir/root-init.log"
disk="$work_dir/root.img"

trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

for tool in truncate mkfs.ext4 debugfs; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing root-init test tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$kernel" ]; then
    echo "missing production kernel: $kernel" >&2
    exit 1
fi
if [ ! -f "$init" ]; then
    echo "missing root-init fixture: $init" >&2
    exit 1
fi

truncate -s 32M "$disk"
mkfs.ext4 -q -F "$disk"
debugfs -w -R "write $init /init" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init mode 0100755" "$disk" \
    >/dev/null 2>&1

if ! timeout -k 2s 15s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m "$memory" \
    -smp 1 \
    -nographic \
    -no-reboot \
    -global virtio-mmio.force-legacy=false \
    -drive file="$disk",if=none,format=raw,readonly=on,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$output" 2>&1; then
    tail -n 120 "$output" >&2
    echo "production kernel failed to run disk-backed /init" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: root /init started pid=0x1' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "production kernel did not start PID 1 from ext4" >&2
    exit 1
fi
if [ "$(grep -cE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "production kernel did not fully reap PID 1 and root resources" >&2
    exit 1
fi
if grep -qE 'BoarOS: (root boot error|scheduler startup/idle error|fatal trap|SBI shutdown failed)' "$output"; then
    tail -n 120 "$output" >&2
    echo "production root boot reported an error" >&2
    exit 1
fi

echo "RISC-V production kernel loaded /init from read-only ext4"
