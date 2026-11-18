#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/reboot-namespace
for shift in 12 14; do
 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT="$shift" -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include -fsanitize=address,undefined tests/host/reboot_namespace_syscall.c kernel/reboot.c -Wl,--gc-sections,--wrap=kernel_reboot_request -o "build/host/reboot-namespace/syscall-$shift"
 ASAN_OPTIONS=detect_leaks=0 "build/host/reboot-namespace/syscall-$shift"
done
