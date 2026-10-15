#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/build/host/string"
mkdir -p "$out"
${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror -fno-builtin \
    -fstrict-aliasing -Wstrict-aliasing=2 \
    -Dmemcpy=boaros_memcpy -Dmemmove=boaros_memmove -Dmemset=boaros_memset \
    -Dmemcmp=boaros_memcmp -Dstrcmp=boaros_strcmp -Dstrcpy=boaros_strcpy \
    -Dstrlen=boaros_strlen -Dstrncmp=boaros_strncmp -Dstrncpy=boaros_strncpy \
    ${STRING_HOST_FLAGS:-} -c "$root/lib/string.c" -o "$out/string.o"
${HOST_CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Werror \
    ${STRING_HOST_FLAGS:-} "$root/tests/host/string.c" "$out/string.o" -o "$out/test"
"$out/test"
