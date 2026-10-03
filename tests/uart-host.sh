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
