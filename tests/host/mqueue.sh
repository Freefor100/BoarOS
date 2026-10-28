#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build/mqueue-pipe-20261028/mq/host}
mkdir -p "$out"
for shift in ${MQUEUE_HOST_SHIFTS:-12 14}; do
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
    -DBOAROS_MAX_CPUS=4 -DBOAROS_LOCK_DEBUG=1 -DBOAROS_PAGE_SHIFT="$shift" \
    -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
    -fsanitize=address,undefined tests/host/mqueue.c kernel/raw_lock.c kernel/sched/park.c \
    -Wl,--gc-sections -o "$out/cases-$shift"
timeout -k 2s 30s "$out/cases-$shift"
done
