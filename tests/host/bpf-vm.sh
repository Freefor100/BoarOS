#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
build_dir=${BPF_VM_HOST_BUILD:-build/host/bpf-vm}
mkdir -p "$build_dir"
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
    ${BPF_VM_SANITIZERS:--fsanitize=address,undefined -fno-omit-frame-pointer} \
    -idirafter include tests/host/bpf_vm.c kernel/bpf/vm.c kernel/bpf/verify.c \
    kernel/bpf/classic.c -o "$build_dir/bpf-vm"
"$build_dir/bpf-vm"

if command -v "${BPF_LLVM_CC:-clang}" >/dev/null 2>&1 && command -v "${BPF_LLVM_OBJCOPY:-llvm-objcopy}" >/dev/null 2>&1; then
    cat > "$build_dir/llvm-vm.c" <<'SOURCE'
__attribute__((noinline)) static unsigned scale(unsigned x)
{
    volatile long value = (signed char)x * 3;
    return value / 2;
}
unsigned filter(const unsigned *context)
{
    unsigned length = *context, sum = 0;
#pragma clang loop unroll(disable)
    for (unsigned i = 0; i < 5; i++)
        sum += scale((length + i) & 31);
    return __builtin_bswap32(sum);
}
SOURCE
    "${BPF_LLVM_CC:-clang}" -target bpf -mcpu=v4 -O2 -c "$build_dir/llvm-vm.c" -o "$build_dir/llvm-vm.o"
    "${BPF_LLVM_OBJCOPY:-llvm-objcopy}" --dump-section ".text=$build_dir/llvm-vm.bin" "$build_dir/llvm-vm.o"
    "$build_dir/bpf-vm" "$build_dir/llvm-vm.bin"
fi
