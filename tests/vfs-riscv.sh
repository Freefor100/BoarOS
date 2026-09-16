#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${VFS_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-vfs-rv"}
recovery_kernel=${VFS_RECOVERY_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-vfs-recovery-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
mkdir -p "$project_root/build/riscv/artifacts"
work_dir=$(mktemp -d "$project_root/build/riscv/artifacts/vfs.XXXXXX")
output="$work_dir/vfs.log"
disk="$work_dir/root.img"
dirty_disk="$work_dir/dirty-root.img"
fixture="$work_dir/init"
large_fixture="$work_dir/large"

trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work_dir"; else echo "test artifacts retained: $work_dir" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM

show_output()
{
    tail -n 120 "$output" >&2
}

for tool in truncate mkfs.ext4 debugfs od cp tr awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing VFS test tool: $tool" >&2
        exit 1
    fi
done

if [ ! -f "$kernel" ]; then
    echo "missing VFS test kernel: $kernel" >&2
    exit 1
fi
if [ ! -f "$recovery_kernel" ]; then
    echo "missing VFS recovery test kernel: $recovery_kernel" >&2
    exit 1
fi

printf '%s' 'BoarOS root init payload for VFS and ELF' >"$fixture"
awk 'BEGIN { for (i = 0; i < 81920; i++) printf "%c", 65 + (i % 26) }' \
    >"$large_fixture"
truncate -s 32M "$disk"
mkfs.ext4 -q -F "$disk"
debugfs -w -R "write $fixture /init" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init mode 0100755" "$disk" \
    >/dev/null 2>&1
debugfs -w -R "ln /init /init-link" "$disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init links_count 2" "$disk" >/dev/null 2>&1
debugfs -w -R "write $large_fixture /large" "$disk" >/dev/null 2>&1

if ! timeout -k 2s 15s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot \
    -drive file="$disk",if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    </dev/null >"$output" 2>&1; then
    show_output
    echo "QEMU failed during VFS ext4 test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: VFS ext4 tests passed' "$output" || true)" -ne 1 ]; then
    show_output
    echo "VFS ext4 test kernel did not report success" >&2
    exit 1
fi

if grep -qF 'BoarOS: VFS test failed' "$output"; then
    show_output
    echo "VFS ext4 test kernel reported a failed case" >&2
    exit 1
fi

cp "$disk" "$dirty_disk"
feature_incompat=$(od -An -tx4 -j1120 -N4 "$dirty_disk" | tr -d ' ')
dirty_feature_incompat=$((0x$feature_incompat | 4))
debugfs -w -R \
    "set_super_value feature_incompat $dirty_feature_incompat" \
    "$dirty_disk" >/dev/null 2>&1
output="$work_dir/vfs-recovery.log"

if ! timeout -k 2s 15s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$recovery_kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot \
    -drive file="$dirty_disk",if=none,format=raw,readonly=on,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    </dev/null >"$output" 2>&1; then
    show_output
    echo "QEMU failed during ext4 recovery rejection test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: VFS ext4 recovery rejection passed' "$output" || true)" -ne 1 ]; then
    show_output
    echo "VFS recovery test kernel did not reject the dirty journal" >&2
    exit 1
fi

if grep -qF 'BoarOS: VFS test failed' "$output"; then
    show_output
    echo "VFS recovery test kernel reported a failed case" >&2
    exit 1
fi

echo "RISC-V VFS ext4 tests passed"
