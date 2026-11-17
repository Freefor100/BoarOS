#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host
for shift in 12 14; do
  cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=$shift \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itests/host/sync -idirafter include tests/host/seccomp.c kernel/seccomp.c kernel/bpf/classic.c -o build/host/seccomp-$shift
  build/host/seccomp-$shift
done
