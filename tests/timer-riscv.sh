#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cases_kernel=${TIMER_CASES_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-timer-cases-rv"}
boot_kernel=${TIMER_BOOT_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-timer-boot-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
cases_output="$output_dir/timer-cases.log"
boot_output="$output_dir/timer-boot.log"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$cases_kernel" ]; then
    echo "missing timer cases kernel: $cases_kernel" >&2
    exit 1
fi
if [ ! -f "$boot_kernel" ]; then
    echo "missing timer boot kernel: $boot_kernel" >&2
    exit 1
fi

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$cases_kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$cases_output" 2>&1; then
    tail -n 80 "$cases_output" >&2
    echo "QEMU failed during timer cases" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: timer cases failures=0x0' "$cases_output" || true)" -ne 1 ]; then
    tail -n 80 "$cases_output" >&2
    echo "timer cases did not report success" >&2
    exit 1
fi

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$boot_kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$boot_output" 2>&1; then
    tail -n 100 "$boot_output" >&2
    echo "QEMU failed during real timer interrupt test" >&2
    exit 1
fi

interrupt_values=$(sed -n \
    's/^BoarOS: timer interrupt ticks=\(0x[0-9a-f][0-9a-f]*\) entries=\(0x[0-9a-f][0-9a-f]*\) failures=0x0$/\1 \2/p' \
    "$boot_output")
interrupt_ticks=${interrupt_values%% *}
interrupt_entries=${interrupt_values#* }
if [ "$(grep -cxF 'BoarOS: timer frequency=0x989680 tick-hz=0x64 period=0x186a0' "$boot_output" || true)" -ne 1 ] ||
    [ -z "$interrupt_values" ] ||
    [ "$interrupt_ticks" = "$interrupt_values" ] ||
    [ "$interrupt_entries" != "${interrupt_entries#* }" ] ||
    [ "$((interrupt_ticks))" -lt 3 ] ||
    [ "$((interrupt_entries))" -lt 3 ] ||
    [ "$((interrupt_entries))" -gt 8 ]; then
    tail -n 100 "$boot_output" >&2
    echo "real timer interrupt test did not reach three ticks" >&2
    exit 1
fi

if grep -qE 'BoarOS: (fatal trap|timer error)' "$boot_output"; then
    tail -n 100 "$boot_output" >&2
    echo "real timer interrupt test reported a fatal error" >&2
    exit 1
fi

echo "RISC-V timer state and interrupt tests passed"
