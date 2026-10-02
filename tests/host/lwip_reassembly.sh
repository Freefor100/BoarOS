#!/bin/sh
# Native pbuf/fragmenter/reassembly fixture; --upstream reproduces patch failures.
set -eu
cd "$(dirname "$0")/../.."
reassembly_source=third_party/lwip
reassembly_define=
if [ "${1-}" = --upstream ]; then
    reassembly_source=references/lwip
    reassembly_define=-DBOAROS_TEST_UPSTREAM_REASS
    shift
fi
mkdir -p build/host
${CC:-cc} -std=c11 -Wall -Wextra -Werror ${CFLAGS:-} \
    $reassembly_define -Itests/host/lwip_reassembly_opts \
    -I"$reassembly_source/src/include" \
    tests/host/lwip_reassembly.c \
    "$reassembly_source/src/core/def.c" \
    "$reassembly_source/src/core/inet_chksum.c" \
    "$reassembly_source/src/core/ip.c" \
    "$reassembly_source/src/core/mem.c" \
    "$reassembly_source/src/core/memp.c" \
    "$reassembly_source/src/core/pbuf.c" \
    "$reassembly_source/src/core/stats.c" \
    "$reassembly_source/src/core/ipv4/ip4_addr.c" \
    "$reassembly_source/src/core/ipv4/ip4_frag.c" \
    -o build/host/lwip-reassembly
build/host/lwip-reassembly "$@"
