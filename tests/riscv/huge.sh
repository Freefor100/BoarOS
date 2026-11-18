#!/bin/sh
set -eu
log=$(mktemp)
trap 'rm -f "$log"' EXIT HUP INT TERM
if ! timeout -k 2s 15s "${QEMU_RISCV64:-qemu-system-riscv64}" -machine virt -bios default \
    -kernel "${HUGE_TEST_KERNEL_RV:-build/riscv/tests/kernel-huge-rv}" -m 512M -smp 1 -nographic -no-reboot >"$log" 2>&1 </dev/null; then
    cat "$log"; exit 1
fi
if ! grep -qxF 'BoarOS: huge leaf lifecycle passed' "$log"; then cat "$log"; exit 1; fi
echo 'RV active 2MiB huge leaf lifecycle passed'
