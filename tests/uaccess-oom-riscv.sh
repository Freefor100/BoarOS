#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
init=${ROOT_INIT_PROGRAM_RV:-"$project_root/build/riscv/tests/user/uaccess-oom-rv"}

if [ ! -f "$kernel" ] || [ ! -f "$init" ]; then
    echo "missing uaccess OOM test artifact" >&2
    exit 1
fi

exec "$project_root/tests/root-init-riscv.sh"
