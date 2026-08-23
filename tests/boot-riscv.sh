#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel="$project_root/kernel-rv"
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
nm_rv=${NM_RV:-nm}
readelf_rv=${READELF_RV:-readelf}
output_dir=$(mktemp -d)
disk="$output_dir/sdcard-rv.img"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM
truncate -s 1M "$disk"

if [ ! -f "$kernel" ]; then
    echo "missing kernel: $kernel" >&2
    exit 1
fi

entry=$("$readelf_rv" -hW "$kernel" | awk '
    $1 == "Entry" && $2 == "point" && $3 == "address:" { print $4 }
')
if [ "$entry" != "0x80200000" ]; then
    "$readelf_rv" -hW "$kernel" >&2
    echo "expected ELF entry 0x80200000, got ${entry:-missing}" >&2
    exit 1
fi

if ! "$readelf_rv" -lW "$kernel" | awk '
    $1 == "LOAD" {
        count++
        if (count == 1)
            first_ok = ($3 == "0xffffffff80000000" &&
                        $4 == "0x0000000080200000")
        if (substr($3, 1, 10) != "0xffffffff" ||
            substr($4, 1, 10) != "0x00000000")
            bad = 1
    }
    END { exit !(count > 0 && first_ok && !bad) }
'; then
    "$readelf_rv" -lW "$kernel" >&2
    echo "expected high-half ELF VMAs with low physical load addresses" >&2
    exit 1
fi

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

hex_in_half_open_range()
{
    LC_ALL=C awk -v value="x${1#0x}" -v start="x$2" -v end="x$3" '
        BEGIN { exit !(value >= start && value < end) }
    '
}

text_start=$(symbol_value __text_start)
text_end=$(symbol_value __text_end)
global_pointer=$(symbol_value '__global_pointer$')
stack_bottom=$(symbol_value __boot_stack_bottom)
stack_top=$(symbol_value __boot_stack_top)
trap_entry=$(symbol_value riscv_trap_entry)

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

    if [ "$(grep -cE '^BoarOS: high-half pc=0x[0-9a-f]+ sp=0x[0-9a-f]+ gp=0x[0-9a-f]+ stvec=0x[0-9a-f]+$' "$output" || true)" -ne 1 ]; then
        cat "$output" >&2
        echo "expected one high-half execution context line" >&2
        exit 1
    fi

    pc=$(sed -n 's/^BoarOS: high-half pc=\(0x[0-9a-f]*\) sp=.*/\1/p' "$output")
    sp=$(sed -n 's/^BoarOS: high-half .* sp=\(0x[0-9a-f]*\) gp=.*/\1/p' "$output")
    gp=$(sed -n 's/^BoarOS: high-half .* gp=\(0x[0-9a-f]*\) stvec=.*/\1/p' "$output")
    stvec=$(sed -n 's/^BoarOS: high-half .* stvec=\(0x[0-9a-f]*\)$/\1/p' "$output")
    if ! hex_in_half_open_range "$pc" "$text_start" "$text_end" ||
        ! hex_in_half_open_range "$sp" "$stack_bottom" "$stack_top" ||
        [ "${gp#0x}" != "$global_pointer" ] ||
        [ "${stvec#0x}" != "$trap_entry" ]; then
        cat "$output" >&2
        echo "high-half context does not match ELF symbols" >&2
        echo "text=$text_start..$text_end stack=$stack_bottom..$stack_top gp=$global_pointer stvec=$trap_entry" >&2
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
        [ "$((page_total))" -le "$((page_available))" ]; then
        cat "$output" >&2
        echo "expected page tables to consume physical pages" >&2
        exit 1
    fi

    sv39_counts=$(sed -n \
        's/^BoarOS: Sv39 root=0x[1-9a-f][0-9a-f]* tables=\(0x[1-9a-f][0-9a-f]*\) leaf4k=\(0x[1-9a-f][0-9a-f]*\) leaf2m=\(0x[1-9a-f][0-9a-f]*\) satp=0x8[0-9a-f]*$/\1 \2 \3/p' \
        "$output")
    sv39_tables=${sv39_counts%% *}
    sv39_rest=${sv39_counts#* }
    sv39_leaf4k=${sv39_rest%% *}
    sv39_leaf2m=${sv39_rest#* }
    if [ -z "$sv39_counts" ] || [ "$sv39_tables" = "$sv39_counts" ] ||
        [ "$sv39_leaf4k" = "$sv39_rest" ] ||
        [ "$sv39_leaf2m" != "${sv39_leaf2m#* }" ] ||
        [ "$((page_total - page_available))" -ne "$((sv39_tables))" ]; then
        cat "$output" >&2
        echo "expected active Sv39 with accounted 4 KiB and 2 MiB leaves" >&2
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
