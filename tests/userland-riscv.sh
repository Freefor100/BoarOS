#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
program=${REAL_USERLAND_RV:-"$project_root/build/riscv/tests/user/real-userland-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
memory=${QEMU_MEMORY:-512M}
work_dir=$(mktemp -d)
output="$work_dir/userland.log"
disk="$work_dir/root.img"
data="$work_dir/data"

trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

for tool in truncate mkfs.ext4 debugfs awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing userland test tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$kernel" ]; then
    echo "missing production kernel: $kernel" >&2
    exit 1
fi
if [ ! -f "$program" ]; then
    echo "missing real userland program: $program" >&2
    exit 1
fi

truncate -s 32M "$disk"
mkfs.ext4 -q -F "$disk"
awk 'BEGIN { for (i = 0; i < 9000; i++) printf "%c", 65 + (i % 26) }' \
    >"$data"
debugfs -w -R "write $program /init" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init mode 0100755" "$disk" \
    >/dev/null 2>&1
debugfs -w -R "write $data /data" "$disk" >/dev/null 2>&1

if ! timeout -k 2s 15s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m "$memory" \
    -smp 1 \
    -nographic \
    -no-reboot \
    -drive file="$disk",if=none,format=raw,readonly=on,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$output" 2>&1; then
    tail -n 120 "$output" >&2
    echo "production kernel failed to run the real userland program" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: real userland stdio ok' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "real userland program did not write through stdio" >&2
    exit 1
fi
if [ "$(grep -cxF 'BoarOS: real userland file checks ok' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "real userland program did not complete the file checks" >&2
    exit 1
fi
if [ "$(grep -cxF 'BoarOS: real userland clock checks ok' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "real userland program did not complete the clock checks" >&2
    exit 1
fi
if [ "$(grep -cE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "real userland program did not exit cleanly with all resources reaped" >&2
    exit 1
fi
if grep -qE 'BoarOS: (root boot error|scheduler startup/idle error|fatal trap|SBI shutdown failed)' "$output"; then
    tail -n 120 "$output" >&2
    echo "real userland boot reported an error" >&2
    exit 1
fi

echo "RISC-V real userland program ran on the production kernel"
