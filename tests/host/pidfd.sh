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
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        tests/host/socket_identity.c kernel/sched/identity.c kernel/pid.c kernel/cpu.c \
        kernel/raw_lock.c tests/host/sync/notifications.c -Wl,--gc-sections -o "$out/socket-identity-$shift"
    "$out/socket-identity-$shift"
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_LOCK_DEBUG=1 -ffunction-sections -fdata-sections \
        -Itests/host/sync -idirafter include -fsanitize=address,undefined \
        tests/host/pidfd.c kernel/pid.c kernel/raw_lock.c kernel/sched/park.c \
        -Wl,--gc-sections -o "$out/owners-$shift"
    timeout -k 1s 10s "$out/owners-$shift"
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_USER_SMP=1 -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_LOCK_DEBUG=1 \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        -fsanitize=address,undefined tests/host/pidfd_files.c fs/files/table.c \
        kernel/pid.c kernel/raw_lock.c kernel/sched/park.c -Wl,--gc-sections \
        -Wl,--wrap=kernel_files_pin,--wrap=kernel_files_release,--wrap=kernel_raw_lock_acquire \
        -o "$out/files-$shift"
    for case in getfd-local open-local nofile-open nofile-getfd identity recheck cleanup-owner exit; do
        timeout -k 1s 10s "$out/files-$shift" "$case"
    done
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_USER_SMP=1 -DBOAROS_PAGE_SHIFT="$shift" -DBOAROS_PAGE_CACHE_READAHEAD_PAGES=0 \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        -fsanitize=address,undefined tests/host/pidfd_close.c tests/host/pidfd_close_unavailable.c \
        fs/open_file.c -Wl,--gc-sections -o "$out/close-$shift"
    timeout -k 1s 10s "$out/close-$shift"
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -ffunction-sections -fdata-sections -Itests/host/sync -idirafter include \
        tests/host/proc_maps.c -Wl,--gc-sections -o "$out/proc-$shift"
    "$out/proc-$shift"
done
