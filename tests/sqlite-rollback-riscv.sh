#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 -B "$root/tests/sqlite-rollback.py" --arch riscv \
    --kernel "${KERNEL_RV:-$root/kernel-rv}" \
    --qemu "${QEMU_RISCV64:-qemu-system-riscv64}" "$@"
