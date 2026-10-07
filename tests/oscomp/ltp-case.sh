#!/bin/sh
# 官方仍逐项遍历；只跳过已核实缺少控制器或输入的辅助程序，不伪造 LTP 结果。
# 原盘hush会把while的正常结束当作errexit；此适配器逐项显式检查失败。
[ "$#" = 5 ] || exit 125
runner=$1
skips=$2
limit=$3
shell=$4
file=$5
name=${file##*/}
[ -r "$skips" ] || exit 125
# 显式诊断排除与缺少控制器的辅助程序分开；默认为空，不制造通过结果。
# 原LA盘hush没有set -f；赋值中按空格拆分，避免把literal排除名展开为路径。
exclusions=${BOAROS_DIAGNOSTIC_EXCLUDE:-}
while [ -n "$exclusions" ]; do
    case "$exclusions" in
        *' '*) excluded=${exclusions%% *}; exclusions=${exclusions#* } ;;
        *) excluded=$exclusions; exclusions= ;;
    esac
    if [ "$name" = "$excluded" ]; then
        echo "BOAROS-CASE EXCLUDE command=$file reason=user-requested-diagnostic-exclusion" >&2
        exit 125
    fi
done
tab=$(printf '\t') || exit 125
exec 3< "$skips" || exit 125
while IFS="$tab" read -r helper source reason <&3; do
    case "$helper" in ''|'#'*) continue ;; esac
    if [ "$name" = "$helper" ]; then
        echo "BOAROS-CASE SKIP command=$file reason=$reason source=$source" >&2
        # 保留原 FAIL 行的非零状态；没有 Summary/TPASS，不会获得通过分。
        exit 125
    fi
done
exec 3<&- || exit 125
exec "$runner" "$limit" "$shell" "$file"
