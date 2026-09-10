#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
objdump=${OBJDUMP_RV:-riscv64-unknown-elf-objdump}
mm_object=${MM_OBJECT_RV:-"$project_root/build/riscv/arch/riscv/mm.o"}
elf_object=${ELF_IMAGE_OBJECT_RV:-"$project_root/build/riscv/arch/riscv/elf_image.o"}
sv39_object=${SV39_OBJECT_RV:-"$project_root/build/riscv/arch/riscv/sv39.o"}

has_fence_i()
{
    object=$1
    symbol=$2
    "$objdump" -d "$object" |
        awk -v symbol="$symbol" '
            $0 ~ "<" symbol ">:" { in_symbol = 1; next }
            in_symbol && /^Disassembly of section / { exit }
            in_symbol && /fence\.i/ { found = 1 }
            END { exit found ? 0 : 1 }
        '
}

for object in "$mm_object" "$elf_object" "$sv39_object"; do
    if [ ! -f "$object" ]; then
        echo "missing instruction-cache test object: $object" >&2
        exit 1
    fi
done

if ! has_fence_i "$mm_object" kernel_mm_resolve_user_fault; then
    echo "demand-populated executable mappings lack fence.i" >&2
    exit 1
fi
if ! has_fence_i "$elf_object" riscv_elf_image_build; then
    echo "loaded executable image lacks fence.i before activation" >&2
    exit 1
fi
if ! has_fence_i "$sv39_object" riscv_sv39_user_resolve_cow; then
    echo "COW executable mappings lack fence.i" >&2
    exit 1
fi
if ! has_fence_i "$sv39_object" riscv_sv39_user_protect_owned_range; then
    echo "mprotect executable mappings lack fence.i" >&2
    exit 1
fi

echo "RISC-V executable mapping instruction synchronization passed"
