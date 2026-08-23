#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${TRAP_RETURN_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-trap-return-rv"}
sie_kernel=${TRAP_RETURN_SIE_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-trap-return-sie-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
objdump=${OBJDUMP_RV:-objdump}
output_dir=$(mktemp -d)
output="$output_dir/trap-return.log"
sie_output="$output_dir/trap-return-sie.log"
full_disassembly="$output_dir/trap-return.dis"
entry_disassembly="$output_dir/riscv-trap-entry.dis"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

show_output()
{
    tail -n 100 "$output" >&2
}

run_kernel()
{
    test_kernel=$1
    test_output=$2

    if [ ! -f "$test_kernel" ]; then
        echo "missing trap return test kernel: $test_kernel" >&2
        exit 1
    fi

    if ! timeout -k 2s 10s "$qemu" \
        -machine virt \
        -bios default \
        -kernel "$test_kernel" \
        -m 512M \
        -smp 1 \
        -nographic \
        -no-reboot \
        </dev/null >"$test_output" 2>&1; then
        tail -n 100 "$test_output" >&2
        echo "QEMU failed during trap return test" >&2
        exit 1
    fi
}

verify_trap_return_sequence()
{
    if ! "$objdump" -d "$kernel" >"$full_disassembly"; then
        echo "failed to disassemble trap return test kernel" >&2
        exit 1
    fi

    sed -n '/<riscv_trap_entry>:/,/^$/p' \
        "$full_disassembly" >"$entry_disassembly"

    address_count=$(grep -cE \
        '[[:space:]]addi[[:space:]]+t1,sp,256$' \
        "$entry_disassembly" || true)
    sc_count=$(grep -cE \
        '[[:space:]]sc\.d[[:space:]]+zero,t0,\(t1\)$' \
        "$entry_disassembly" || true)
    sepc_count=$(grep -cE \
        '[[:space:]]csrw[[:space:]]+sepc,t0$' \
        "$entry_disassembly" || true)
    sret_count=$(grep -cE \
        '[[:space:]]sret$' "$entry_disassembly" || true)

    if [ "$address_count" -ne 1 ] || [ "$sc_count" -ne 1 ] || \
        [ "$sepc_count" -ne 1 ] || [ "$sret_count" -ne 1 ]; then
        cat "$entry_disassembly" >&2
        echo "trap return reservation-clearing sequence is missing or ambiguous" >&2
        exit 1
    fi

    address_line=$(grep -nE \
        '[[:space:]]addi[[:space:]]+t1,sp,256$' \
        "$entry_disassembly" | cut -d: -f1)
    sc_line=$(grep -nE \
        '[[:space:]]sc\.d[[:space:]]+zero,t0,\(t1\)$' \
        "$entry_disassembly" | cut -d: -f1)
    sepc_line=$(grep -nE \
        '[[:space:]]csrw[[:space:]]+sepc,t0$' \
        "$entry_disassembly" | cut -d: -f1)
    sret_line=$(grep -nE \
        '[[:space:]]sret$' "$entry_disassembly" | cut -d: -f1)

    if [ "$address_line" -ge "$sc_line" ] || \
        [ "$sc_line" -ge "$sepc_line" ] || \
        [ "$sepc_line" -ge "$sret_line" ]; then
        cat "$entry_disassembly" >&2
        echo "trap return reservation-clearing sequence is out of order" >&2
        exit 1
    fi
}

verify_trap_return_sequence

run_kernel "$kernel" "$output"
run_kernel "$sie_kernel" "$sie_output"

if [ "$(grep -cE '^BoarOS: trap return breakpoint scause=0x3 sepc=0x[0-9a-f]+ sstatus=0x[0-9a-f]+ frame=0x[0-9a-f]+ handler=0x0 registers=0x0$' "$output" || true)" -ne 1 ]; then
    show_output
    echo "breakpoint trap did not preserve and return the complete context" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: trap return software-interrupt scause=0x8000000000000001 sepc=0x[0-9a-f]+ sstatus=0x[0-9a-f]+ frame=0x[0-9a-f]+ handler=0x0 registers=0x0$' "$output" || true)" -ne 1 ]; then
    show_output
    echo "software interrupt did not preserve, acknowledge, and return the context" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: invalid trap return sstatus=0x[0-9a-f]+ sepc=0x[0-9a-f]+$' "$output" || true)" -ne 1 ]; then
    show_output
    echo "invalid trap return state was not rejected" >&2
    exit 1
fi

if grep -qF 'BoarOS: trap bad return escaped validation' "$output"; then
    show_output
    echo "execution escaped invalid trap return validation" >&2
    exit 1
fi

if [ "$(grep -cE '^BoarOS: invalid trap return sstatus=0x[0-9a-f]+ sepc=0x[0-9a-f]+$' "$sie_output" || true)" -ne 1 ]; then
    tail -n 100 "$sie_output" >&2
    echo "SIE-enabled trap return state was not rejected" >&2
    exit 1
fi

if grep -qF 'BoarOS: trap SIE bad return escaped validation' "$sie_output"; then
    tail -n 100 "$sie_output" >&2
    echo "execution escaped SIE return validation" >&2
    exit 1
fi

echo "RISC-V trap return test passed"
