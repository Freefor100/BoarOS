#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
init=${ROOT_INIT_PROGRAM_RV:-"$project_root/build/riscv/tests/user/root-init-rv"}
stage2=${ROOT_EXEC_STAGE2_RV:-"$project_root/build/riscv/tests/user/root-exec-stage2-rv"}
stage3=${ROOT_EXEC_STAGE3_RV:-"$project_root/build/riscv/tests/user/root-exec-stage3-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
memory=${QEMU_MEMORY:-512M}
root_boot_error_status=${ROOT_BOOT_ERROR_STATUS:-}
virtio_mmio_force_legacy=${VIRTIO_MMIO_FORCE_LEGACY:-}
work_dir=$(mktemp -d)
output="$work_dir/root-init.log"
disk="$work_dir/root.img"
data="$work_dir/data"
script="$work_dir/script"

trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

for tool in truncate mkfs.ext4 debugfs awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing root-init test tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$kernel" ]; then
    echo "missing production kernel: $kernel" >&2
    exit 1
fi
for fixture in "$init" "$stage2" "$stage3"; do
    if [ ! -f "$fixture" ]; then
        echo "missing root exec fixture: $fixture" >&2
        exit 1
    fi
done

truncate -s 32M "$disk"
mkfs.ext4 -q -F "$disk"
awk 'BEGIN { for (i = 0; i < 9000; i++) printf "%c", 65 + (i % 26) }' \
    >"$data"
printf '%s\n' '#!/bin/sh' >"$script"
debugfs -w -R "write $init /init" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init mode 0100755" "$disk" \
    >/dev/null 2>&1
debugfs -w -R "write $stage2 /stage2" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /stage2 mode 0100755" "$disk" \
    >/dev/null 2>&1
debugfs -w -R "write $stage3 /stage3" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /stage3 mode 0100755" "$disk" \
    >/dev/null 2>&1
debugfs -w -R "write $data /data" "$disk" >/dev/null 2>&1
debugfs -w -R "write $script /script" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /script mode 0100755" "$disk" \
    >/dev/null 2>&1

set -- "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m "$memory" \
    -smp 1 \
    -nographic \
    -no-reboot
if [ -n "$virtio_mmio_force_legacy" ]; then
    set -- "$@" -global "virtio-mmio.force-legacy=$virtio_mmio_force_legacy"
fi
set -- "$@" \
    -drive file="$disk",if=none,format=raw,readonly=on,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0

if ! timeout -k 2s 15s "$@" </dev/null >"$output" 2>&1; then
    tail -n 120 "$output" >&2
    echo "production kernel failed to run disk-backed /init" >&2
    exit 1
fi

if [ -n "$root_boot_error_status" ]; then
    if [ "$(grep -cxF "BoarOS: root boot error status=$root_boot_error_status" "$output" || true)" -ne 1 ]; then
        tail -n 120 "$output" >&2
        echo "production kernel did not report the expected root boot error" >&2
        exit 1
    fi
    if grep -qE 'BoarOS: root /init started|BoarOS: PID 1 exited' "$output"; then
        tail -n 120 "$output" >&2
        echo "production kernel started a user process after root boot failed" >&2
        exit 1
    fi
    echo "RISC-V root-boot cleanup retried VMA teardown"
    exit 0
fi

if [ "$(grep -cxF 'BoarOS: root /init started pid=0x1' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "production kernel did not start PID 1 from ext4" >&2
    exit 1
fi
# The stdio markers only apply to the default /init fixture; the OOM
# variant substitutes a different program.
if [ -z "${ROOT_INIT_PROGRAM_RV:-}" ]; then
    if [ "$(grep -cxF 'BoarOS: root-init stdio write ok' "$output" || true)" -ne 1 ]; then
        tail -n 120 "$output" >&2
        echo "PID 1 did not write through the console descriptor" >&2
        exit 1
    fi
    if [ "$(grep -cxF 'BoarOS: stage2 stdio write ok' "$output" || true)" -ne 1 ]; then
        tail -n 120 "$output" >&2
        echo "stage2 did not keep the console descriptors across exec" >&2
        exit 1
    fi
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
