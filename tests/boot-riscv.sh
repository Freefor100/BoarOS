#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel="$project_root/kernel-rv"
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
output_dir=$(mktemp -d)
disk="$output_dir/sdcard-rv.img"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM
truncate -s 1M "$disk"

if [ ! -f "$kernel" ]; then
    echo "missing kernel: $kernel" >&2
    exit 1
fi

run_case()
{
    memory=$1
    expected_size=$2
    output="$output_dir/boot-$memory.log"

    echo "RISC-V boot test: memory=$memory"

    if ! timeout -k 2s 10s "$qemu" \
        -machine virt \
        -bios default \
        -kernel "$kernel" \
        -m "$memory" \
        -smp 1 \
        -nographic \
        -no-reboot \
        -drive file="$disk",if=none,format=raw,id=x0 \
        -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
        -device virtio-net-device,netdev=net \
        -netdev user,id=net \
        -rtc base=utc </dev/null >"$output" 2>&1; then
        cat "$output" >&2
        echo "QEMU failed for -m $memory" >&2
        exit 1
    fi

    if [ "$(grep -cF 'BoarOS: booted ' "$output" || true)" -ne 1 ]; then
        cat "$output" >&2
        echo "expected one BoarOS boot line for -m $memory" >&2
        exit 1
    fi

    if ! grep -Eq 'BoarOS: booted hart=0x0 dtb=0x[0-9a-f]+$' "$output"; then
        cat "$output" >&2
        echo "boot line did not expose the OpenSBI handoff registers" >&2
        exit 1
    fi

    expected_memory="BoarOS: memory base=0x80000000 size=$expected_size"
    if [ "$(grep -cxF "$expected_memory" "$output" || true)" -ne 1 ]; then
        cat "$output" >&2
        echo "expected one DTB memory line: $expected_memory" >&2
        exit 1
    fi

    if ! grep -Eq '^BoarOS: memory layout reserved=0x[1-9a-f][0-9a-f]* usable=0x[1-9a-f][0-9a-f]*$' "$output"; then
        cat "$output" >&2
        echo "expected a non-empty boot memory layout" >&2
        exit 1
    fi

    if ! grep -Eq '^BoarOS: first reserved base=0x80000000 size=0x[1-9a-f][0-9a-f]*$' "$output"; then
        cat "$output" >&2
        echo "expected the OpenSBI reservation at RAM base" >&2
        exit 1
    fi

    if ! grep -Eq '^BoarOS: first usable base=0x[1-9a-f][0-9a-f]* size=0x[1-9a-f][0-9a-f]*$' "$output"; then
        cat "$output" >&2
        echo "expected a concrete first usable range" >&2
        exit 1
    fi

    page_counts=$(sed -n \
        's/^BoarOS: physical pages total=\(0x[1-9a-f][0-9a-f]*\) available=\(0x[1-9a-f][0-9a-f]*\)$/\1 \2/p' \
        "$output")
    page_total=${page_counts%% *}
    page_available=${page_counts#* }
    if [ -z "$page_counts" ] || [ "$page_total" = "$page_counts" ] ||
        [ "$page_available" != "${page_available#* }" ] ||
        [ "$page_total" != "$page_available" ]; then
        cat "$output" >&2
        echo "expected one initialized physical page allocator" >&2
        exit 1
    fi
}

run_case 512M 0x20000000
run_case 1G 0x40000000

dtb_512=$(sed -n 's/^BoarOS: booted .* dtb=\(0x[0-9a-f]*\)$/\1/p' "$output_dir/boot-512M.log")
dtb_1g=$(sed -n 's/^BoarOS: booted .* dtb=\(0x[0-9a-f]*\)$/\1/p' "$output_dir/boot-1G.log")

if [ "$dtb_512" = "$dtb_1g" ]; then
    echo "DTB address did not change with guest memory size: $dtb_512" >&2
    exit 1
fi

echo "RISC-V boot passed: DTB moved from $dtb_512 to $dtb_1g"
