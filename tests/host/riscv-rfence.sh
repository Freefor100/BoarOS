#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
out=${RISCV_RFENCE_HOST_BUILD:-build/host/riscv-rfence}
mkdir -p "$out"
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=8 \
    -DBOAROS_PAGE_SHIFT=12 -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/riscv_rfence.c arch/riscv/smp_sync.c -o "$out/riscv-rfence"
"$out/riscv-rfence"
