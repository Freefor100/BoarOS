#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cd "$root"
for shift in 12 14; do
    cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=undefined \
        -DBOAROS_HOST_IRQ_STATE -DBOAROS_MAX_CPUS=2 -DBOAROS_PAGE_SHIFT="$shift" \
        -Itests/host/random -idirafter include tests/host/rtnetlink_ipv6_ra.c -o "$work/encoder"
    "$work/encoder"
done
