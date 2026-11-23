#!/bin/sh
# 官方逐项遍历时应用共享跳过表，其余项目交给监督程序。
# 原盘hush会把while的正常结束当作errexit；此适配器逐项显式检查失败。
[ "$#" = 5 ] || exit 125
runner=$1
skips=$2
limit=$3
shell=$4
file=$5
name=${file##*/}
[ -r "$skips" ] || exit 125
# 显式诊断排除独立于共享跳过表，默认为空。
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
if [ "$name" = df01.sh ]; then
    # 原脚本使用 GNU stat、df 的选项。工具目录只进入该测例的环境。
    PATH=/boaros-tools:$PATH
    export PATH
    if [ "${BOAROS_EVAL_ARCH:-}" = loongarch ]; then
        # 原计时函数通过变量名读取调用者的 local sec，LA 发布盘的 hush 无法解析它。
        exec "$runner" -i "$limit" /boaros-tools/dash "$file"
    fi
fi
if [ "$name" = keyctl02 ]; then
    # 上游用这两项判断是否要给实时内核的线程回收留出额外时间。
    KCONFIG_PATH="${TMPDIR:-/tmp}/boaros-keyctl02-kconfig"
    printf '%s\n' '# CONFIG_PREEMPT_RT is not set' \
        '# CONFIG_PREEMPT_RT_FULL is not set' > "$KCONFIG_PATH" || exit 125
    export KCONFIG_PATH
fi
exec "$runner" "$limit" "$shell" "$file"
