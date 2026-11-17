#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host
for shift in 12 14; do
  cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_PAGE_SHIFT=$shift \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itests/host/sync -idirafter include tests/host/seccomp.c kernel/seccomp.c kernel/bpf/classic.c -o build/host/seccomp-$shift
  build/host/seccomp-$shift
  cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections \
    -DBOAROS_MAX_CPUS=2 -DBOAROS_USER_SMP=1 -DBOAROS_PAGE_SHIFT=$shift -Itests/host/sync -idirafter include \
    tests/host/seccomp_sync.c kernel/seccomp.c kernel/sched/seccomp.c kernel/sched/exec.c \
    kernel/bpf/classic.c -Wl,--gc-sections -fsanitize=address,undefined \
    -o build/host/seccomp-sync-$shift
  build/host/seccomp-sync-$shift
done
