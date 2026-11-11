#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/bpf
for shift in ${BPF_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror \
        -DBOAROS_MAX_CPUS=1 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/bpf_test_run.c tests/host/bpf_test_run_unavailable.c \
        kernel/bpf/program.c kernel/bpf/runtime.c kernel/bpf/verify.c kernel/bpf/vm.c \
        ${BPF_HOST_FLAGS:-} -Wl,--gc-sections -o "build/host/bpf/test-run-$shift"
    timeout 30s "build/host/bpf/test-run-$shift"
done
