#!/bin/sh
# 官方仍逐项遍历；只跳过已核实缺少控制器或输入的辅助程序，不伪造 LTP 结果。
set -eu
[ "$#" = 5 ] || exit 125
runner=$1
skips=$2
limit=$3
shell=$4
file=$5
name=${file##*/}
[ -r "$skips" ] || exit 125
# 显式诊断排除与缺少控制器的辅助程序分开；默认为空，不制造通过结果。
set -f
for excluded in ${BOAROS_DIAGNOSTIC_EXCLUDE:-}; do
    if [ "$name" = "$excluded" ]; then
        echo "BOAROS-CASE EXCLUDE command=$file reason=user-requested-diagnostic-exclusion" >&2
        exit 125
    fi
done
tab=$(printf '\t')
while IFS="$tab" read -r helper source reason; do
    case "$helper" in ''|'#'*) continue ;; esac
    if [ "$name" = "$helper" ]; then
        echo "BOAROS-CASE SKIP command=$file reason=$reason source=$source" >&2
        # 保留原 FAIL 行的非零状态；没有 Summary/TPASS，不会获得通过分。
        exit 125
    fi
done < "$skips"
exec "$runner" "$limit" "$shell" "$file"
