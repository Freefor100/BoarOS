#!/bin/sh
# 只给官方脚本的单项执行点接监督；不换目录、不改参数、不生成测例清单。
set -e
if [ "$#" -ne 2 ]; then exit 125; fi
count=$(grep -c '^[[:space:]]*"\$file"[[:space:]]*$' "$1" || :)
if [ "$count" != 1 ]; then
    echo 'BOAROS-EVAL LTP script shape changed; refusing to replace the flow' >&2
    exit 125
fi
sed 's@^[[:space:]]*"\$file"[[:space:]]*$@    sh /tmp/boaros-ltp-case.sh /tmp/boaros-case /tmp/boaros-ltp-skips.tsv "$BOAROS_LTP_CASE_TIMEOUT" "$BOAROS_CASE_SHELL" "$file"@' "$1" > "$2"
