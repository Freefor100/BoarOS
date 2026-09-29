#!/bin/sh
set -eu
project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
program=${MULTI_MOUNT_RV:-"$project_root/build/riscv/tests/user/multi-mount-rv"}
readonly_program=${MULTI_MOUNT_READONLY_RV:-"$project_root/build/riscv/tests/user/multi-mount-readonly-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
memory=${QEMU_MEMORY:-512M}
mkdir -p "$project_root/build/riscv/artifacts"
work_dir=$(mktemp -d "$project_root/build/riscv/artifacts/multi-block.XXXXXX")
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work_dir"; else echo "test artifacts retained: $work_dir" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM
for artifact in "$kernel" "$program" "$readonly_program"; do
    test -f "$artifact" || { echo "missing artifact: $artifact" >&2; exit 1; }
done
printf ROOT > "$work_dir/root-value"
printf SECOND > "$work_dir/second-value"

run_boot()
{
    phase=$1
    output="$work_dir/$mode-$phase.log"
    root_disk="$work_dir/$mode-$phase-root.img"
    if [ "$phase" = write ]; then
        init=$program
        readonly=off
        marker='BoarOS: multi-mount write and remount checks ok'
    else
        init=$readonly_program
        readonly=on
        marker='BoarOS: multi-mount read-only persistence checks ok'
    fi
    truncate -s 32M "$root_disk"
    mkfs.ext4 -q -F "$root_disk"
    debugfs -w -R "write $init /init" "$root_disk" >/dev/null 2>&1
    debugfs -w -R 'set_inode_field /init mode 0100755' "$root_disk" >/dev/null 2>&1
    debugfs -w -R "write $work_dir/root-value /identity" "$root_disk" >/dev/null 2>&1
    if ! timeout -k 2s 45s "$qemu" -machine virt -bios default -kernel "$kernel" \
        -m "$memory" -smp 1 -nographic -no-reboot \
        -global "virtio-mmio.force-legacy=$mode" \
        -drive "file=$root_disk,if=none,format=raw,id=root" \
        -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
        -drive "file=$second_disk,if=none,format=raw,readonly=$readonly,id=secondary" \
        -device virtio-blk-device,drive=secondary,bus=virtio-mmio-bus.1 \
        </dev/null > "$output" 2>&1; then
        tail -n 100 "$output" >&2
        exit 1
    fi
    if [ "$(grep -cxF "$marker" "$output" || true)" -ne 1 ] ||
       [ "$(grep -cE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$output" || true)" -ne 1 ]; then
        tail -n 100 "$output" >&2
        exit 1
    fi
    awk '
    /^BoarOS: block device=0xfc00 irq=0x[1-9a-f][0-9a-f]*$/ {first++; a=$4}
    /^BoarOS: block device=0xfc10 irq=0x[1-9a-f][0-9a-f]*$/ {second++; b=$4}
    END {exit !(first == 1 && second == 1 && a != b)}
    ' "$output"
    if grep -qE 'BoarOS: (root boot error|fatal trap|root finish failure|block timeout)' "$output"; then
        cat "$output" >&2
        exit 1
    fi
    python3 "$project_root/tests/check-stack-report.py" "$output"
    e2fsck -fn "$second_disk" > "$work_dir/$mode-$phase-fsck.log" 2>&1 || {
        cat "$work_dir/$mode-$phase-fsck.log" >&2; exit 1;
    }
}

for mode in true false; do
    second_disk="$work_dir/$mode-secondary.img"
    truncate -s 32M "$second_disk"
    mkfs.ext4 -q -F "$second_disk"
    debugfs -w -R "write $work_dir/second-value /identity" "$second_disk" >/dev/null 2>&1
    run_boot write
    run_boot readonly
    printf 'PASS: real second-disk mount, nested tmpfs, persistence, read-only reboot and teardown (legacy=%s)\n' "$mode"
done
