#!/bin/sh
set -eu
mkdir -p build/host/mm
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 -ffunction-sections -fdata-sections \
    -DBOAROS_PAGE_SHIFT=12 -DMM_TEST_ELF_SOURCE=1 -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/elf_layout.c tests/host/mm/context.c tests/host/mm/file_unavailable.c \
    fs/vfs.c fs/tmpfs.c fs/open_file.c fs/file_mapping.c fs/record_lock.c \
    kernel/elf64.c kernel/elf64_source.c kernel/elf_image.c kernel/read_source.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c kernel/sched/sync.c kernel/sched/park.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    -Wl,--gc-sections -o build/host/mm/elf-layout
timeout 30s build/host/mm/elf-layout
