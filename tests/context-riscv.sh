#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${CONTEXT_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-context-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
objdump=${OBJDUMP_RV:-riscv64-unknown-elf-objdump}
context_object=${CONTEXT_OBJECT_RV:-"$project_root/build/riscv/arch/riscv/context_switch.o"}
output_dir=$(mktemp -d)
output="$output_dir/context.log"
disassembly="$output_dir/context.dis"
switch_disassembly="$output_dir/switch.dis"
trampoline_disassembly="$output_dir/trampoline.dis"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing context test kernel: $kernel" >&2
    exit 1
fi
if [ ! -f "$context_object" ]; then
    echo "missing context object: $context_object" >&2
    exit 1
fi

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$output" 2>&1; then
    tail -n 80 "$output" >&2
    echo "QEMU failed during context cases" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: context cases failures=0x0' "$output" || true)" -ne 1 ]; then
    tail -n 80 "$output" >&2
    echo "context cases did not report success" >&2
    exit 1
fi

"$objdump" -dr "$context_object" >"$disassembly"
sed -n '/<riscv_context_switch>:/,/<riscv_kernel_thread_trampoline>:/p' \
    "$disassembly" >"$switch_disassembly"
sed -n '/<riscv_kernel_thread_trampoline>:/,$p' \
    "$disassembly" >"$trampoline_disassembly"

for pair in \
    ra:0 sp:8 tp:16 s0:24 s1:32 s2:40 s3:48 s4:56 s5:64 \
    s6:72 s7:80 s8:88 s9:96 s10:104 s11:112
do
    register=${pair%%:*}
    offset=${pair#*:}
    if [ "$(grep -Ec "[[:space:]]sd[[:space:]]+$register,$offset\\(a0\\)" \
        "$switch_disassembly" || true)" -ne 1 ] ||
        [ "$(grep -Ec "[[:space:]]ld[[:space:]]+$register,$offset\\(a1\\)" \
        "$switch_disassembly" || true)" -ne 1 ]; then
        cat "$switch_disassembly" >&2
        echo "context switch does not preserve $register at $offset" >&2
        exit 1
    fi
done

if grep -Eq '[[:space:]](sd|ld)[[:space:]]+(a[0-7]|t[0-6]|gp),' \
    "$switch_disassembly"; then
    cat "$switch_disassembly" >&2
    echo "context switch preserves caller-saved or shared registers" >&2
    exit 1
fi

enable_line=$(grep -n 'csrsi[[:space:]]*sstatus,2' \
    "$trampoline_disassembly" | cut -d: -f1)
entry_line=$(grep -n 'jalr[[:space:]]*s0' \
    "$trampoline_disassembly" | cut -d: -f1)
disable_line=$(grep -n 'csrci[[:space:]]*sstatus,2' \
    "$trampoline_disassembly" | cut -d: -f1)
exit_line=$(grep -n 'R_RISCV_CALL_PLT[[:space:]]*kernel_thread_exit' \
    "$trampoline_disassembly" | cut -d: -f1)
if [ -z "$enable_line" ] || [ -z "$entry_line" ] ||
    [ -z "$disable_line" ] || [ -z "$exit_line" ] ||
    [ "$enable_line" -ge "$entry_line" ] ||
    [ "$entry_line" -ge "$disable_line" ] ||
    [ "$disable_line" -ge "$exit_line" ]; then
    cat "$trampoline_disassembly" >&2
    echo "thread trampoline interrupt/entry/exit order is invalid" >&2
    exit 1
fi

echo "RISC-V context cases passed"
