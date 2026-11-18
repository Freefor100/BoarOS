#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/host/hugetlb
# The providers model hstate counters; no pool capacity is invented in procfs.
for shift in 12 14; do
    ${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -DBOAROS_MAX_CPUS=2 \
        -DBOAROS_PAGE_SHIFT=$shift -ffunction-sections -fdata-sections \
        ${HUGETLB_TEST_CFLAGS:-} -Itests/host/sync -idirafter include \
        tests/host/proc_hugetlb.c -Wl,--gc-sections -o build/host/hugetlb/proc-$shift
    build/host/hugetlb/proc-$shift
done
