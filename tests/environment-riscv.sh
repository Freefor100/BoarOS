#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "$root/build/riscv/environment-run.XXXXXX")
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "environment artifacts: $work" >&2; fi' EXIT
compiler="$root/build/riscv/musl-root/bin/musl-gcc"
flags=
if "$compiler" -fno-link-libatomic -E -x c /dev/null >/dev/null 2>&1; then flags=-fno-link-libatomic; fi
"$compiler" $flags -static -O2 -Wall -Wextra -Werror \
    "$root/tests/workloads/environment.c" -o "$work/init"
truncate -s 32M "$work/root.img"
mkfs.ext4 -q -F -b 4096 "$work/root.img"
debugfs -w -R "write $work/init /init" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /init mode 0100755' "$work/root.img" >/dev/null 2>&1
timeout -k 2s 60s "${QEMU_RISCV64:-qemu-system-riscv64}" -machine virt -bios default \
    -kernel "${KERNEL_RV:-$root/kernel-rv}" -m 512M -smp 1 -nographic -no-reboot \
    -drive "file=$work/root.img,if=none,format=raw,id=root" \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 >"$work/output" 2>&1
if ! rg -q 'ENV PASS all' "$work/output" || ! rg -q 'status=0x0' "$work/output"; then
    tail -n 35 "$work/output";exit 1
fi
rg 'ENV PASS|heap-live=' "$work/output"
