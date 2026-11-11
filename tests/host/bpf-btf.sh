#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/bpf
for shift in ${BPF_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror \
        -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/bpf_btf.c kernel/bpf/btf.c ${BPF_HOST_FLAGS:-} \
        -Wl,--gc-sections -o "build/host/bpf/btf-$shift"
    timeout 30s "build/host/bpf/btf-$shift"
done
