#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
build=${LWEXT4_PORT_HOST_BUILD:-build/host/lwext4-port}
mkdir -p "$build"
for shift in ${LWEXT4_PORT_HOST_SHIFTS:-12 14}; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -ffunction-sections -fdata-sections -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
        -Ithird_party/lwext4/include -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/lwext4_port.c tests/host/mm/context.c tests/host/sync/notifications.c \
        kernel/cpu.c kernel/raw_lock.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c fs/lwext4_port.c -Wl,--gc-sections,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_heap_release \
        -o "$build/port-$shift"
    timeout 20s "$build/port-$shift"
    ${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread \
        -Wno-unused-but-set-variable -Wno-stringop-truncation \
        -ffunction-sections -fdata-sections -DBOAROS_MAX_CPUS=1 -DBOAROS_PAGE_SHIFT="$shift" \
        -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_USE_USER_MALLOC=1 \
        -include tests/host/lwext4_memory.h \
        -Ithird_party/lwext4/include -Itests/host/mm -Itests/host/sync -idirafter include \
        tests/host/lwext4_port_serial.c tests/host/mm/context.c tests/host/sync/notifications.c \
        kernel/cpu.c kernel/raw_lock.c kernel/sched/sync.c kernel/sched/park.c \
        kernel/physical_page.c mm/heap.c fs/lwext4_port.c third_party/lwext4/src/ext4.c \
        -Wl,--gc-sections,--wrap=kernel_raw_lock_acquire,--wrap=kernel_raw_lock_release \
        -o "$build/serial-$shift"
    timeout 20s "$build/serial-$shift"
done
