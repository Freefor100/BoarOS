#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/host/uart
${HOST_CC:-cc} -std=gnu11 -O2 -g -Wall -Wextra -Werror \
    -DBOAROS_PAGE_SHIFT=12 -idirafter include \
    -fsanitize=address,undefined tests/host/uart_tty_test.c \
    -o build/host/uart/transport
build/host/uart/transport
${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -idirafter include \
    tests/host/uart_emergency.c \
    -o build/host/uart/emergency
build/host/uart/emergency


# Actual fs/tty.c enforces destroy/drain; no always-success core teardown stub.
${HOST_CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -DBOAROS_PAGE_SHIFT=12 -Itests/host/uart_startup -Itests/host/random \
    -idirafter include -include tests/host/uart_startup_mmio.h \
    -fsanitize=address,undefined tests/host/uart_startup_owner.c \
    arch/riscv/uart_tty.c drivers/serial/ns16550.c fs/tty.c -o build/host/uart/startup-owner
for uart_case in irq-failure worker-failure initial-busy published-stop; do
    build/host/uart/startup-owner "$uart_case"
done
