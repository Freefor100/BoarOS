#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${SQLITE_SUPERVISOR_CC:-}" ]; then
    set -- --cc "$SQLITE_SUPERVISOR_CC" "$@"
fi
exec python3 -B "$root/tests/sqlite-rollback.py" --arch riscv \
    --kernel "${KERNEL_RV:-$root/kernel-rv}" \
    --qemu "${QEMU_RISCV64:-qemu-system-riscv64}" "$@"
