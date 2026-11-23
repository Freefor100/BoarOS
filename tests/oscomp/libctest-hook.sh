#!/bin/sh
# 发布盘上的 run-static.sh 和 run-dynamic.sh 缺少 shebang。
set -e
[ "$#" = 2 ] || exit 125
static=$(grep -c '^[[:space:]]*\./run-static\.sh[[:space:]]*$' "$1" || :)
dynamic=$(grep -c '^[[:space:]]*\./run-dynamic\.sh[[:space:]]*$' "$1" || :)
if [ "$static" != 1 ] || [ "$dynamic" != 1 ]; then
    echo 'BOAROS-EVAL libc-test script shape changed; refusing to replace the flow' >&2
    exit 125
fi
sed 's@^[[:space:]]*\./run-static\.sh[[:space:]]*$@"$BOAROS_CASE_SHELL" sh ./run-static.sh@; s@^[[:space:]]*\./run-dynamic\.sh[[:space:]]*$@"$BOAROS_CASE_SHELL" sh ./run-dynamic.sh@' "$1" > "$2"
