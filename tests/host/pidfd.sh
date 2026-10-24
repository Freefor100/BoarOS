#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build/host/pidfd}
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude tests/pid-detached-host.c kernel/pid.c -o "$out/detached"
"$out/detached"
cc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude tests/pid-object-host.c kernel/pid.c -o "$out/objects"
"$out/objects"
for shift in 12 14; do
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_LOCK_DEBUG=1 -ffunction-sections -fdata-sections \
        -Itests/host/sync -idirafter include -fsanitize=address,undefined \
        tests/host/pidfd.c kernel/pid.c kernel/raw_lock.c kernel/sched/park.c \
        -Wl,--gc-sections -o "$out/owners-$shift"
    timeout -k 1s 10s "$out/owners-$shift"
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        tests/host/proc_maps.c -Wl,--gc-sections -o "$out/proc-$shift"
    "$out/proc-$shift"
done
