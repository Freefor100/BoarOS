#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${HIGH_HALF_TRAP_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-high-half-trap-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
nm_rv=${NM_RV:-nm}
output_dir=$(mktemp -d)
output="$output_dir/high-half-trap.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

show_output()
{
    tail -n 80 "$output" >&2
}

symbol_value()
{
    "$nm_rv" -n "$kernel" | awk -v wanted="$1" '
        $3 == wanted {
            if (found)
                exit 1
            value = $1
            found = 1
        }
        END {
            if (!found)
                exit 1
            print value
        }
    '
}

if [ ! -f "$kernel" ]; then
    echo "missing high-half trap test kernel: $kernel" >&2
    exit 1
fi

expected_breakpoint=$(symbol_value trap_test_breakpoint)
expected_stvec=$(symbol_value riscv_trap_entry)
expected_return_sepc=$(symbol_value trap_test_software_interrupt_resume)

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot \
    </dev/null >"$output" 2>&1; then
    show_output
    echo "QEMU failed during high-half trap test" >&2
    exit 1
fi

breakpoint=$(sed -n \
    's/^BoarOS: high-half trap breakpoint=\(0x[0-9a-f][0-9a-f]*\)$/\1/p' \
    "$output")
stvec=$(sed -n \
    's/^BoarOS: high-half .* stvec=\(0x[0-9a-f][0-9a-f]*\)$/\1/p' \
    "$output")
actual_sepc=$(sed -n \
    's/^BoarOS: fatal trap scause=0x3 sepc=\(0x[0-9a-f]*\) stval=.*/\1/p' \
    "$output")
return_sepc=$(sed -n \
    's/^BoarOS: high-half trap return scause=0x8000000000000001 sepc=\(0x[0-9a-f]*\) frame=.*/\1/p' \
    "$output")
return_frame=$(sed -n \
    's/^BoarOS: high-half trap return .* frame=\(0x[0-9a-f]*\) handler=0x0 registers=0x0$/\1/p' \
    "$output")

if [ "${breakpoint#0x}" != "$expected_breakpoint" ] ||
    [ "${stvec#0x}" != "$expected_stvec" ] ||
    [ "$actual_sepc" != "$breakpoint" ] ||
    [ "${return_sepc#0x}" != "$expected_return_sepc" ]; then
    show_output
    echo "high-half trap context does not match ELF symbols" >&2
    echo "breakpoint=$expected_breakpoint stvec=$expected_stvec" >&2
    exit 1
fi

case $return_frame:$return_sepc:$actual_sepc in
    0xffffffff[89a-f][0-9a-f]*:0xffffffff[89a-f][0-9a-f]*:0xffffffff[89a-f][0-9a-f]*) ;;
    *)
        show_output
        echo "trap return or breakpoint did not use the Sv39 high half" >&2
        exit 1
        ;;
esac

case $actual_sepc in
    0xffffffff[89a-f][0-9a-f]*) ;;
    *)
        show_output
        echo "breakpoint trap did not originate in the Sv39 high half" >&2
        exit 1
        ;;
esac

if grep -qF 'BoarOS: high-half trap returned' "$output"; then
    show_output
    echo "execution continued after the fatal high-half trap" >&2
    exit 1
fi

echo "RISC-V high-half trap passed: breakpoint at $actual_sepc"
