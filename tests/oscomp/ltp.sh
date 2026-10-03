#!/bin/sh
# 仅用于诊断；清单、控制脚本、helper 与结果判断均来自原镜像。
set -u
if [ "$#" -ne 4 ]; then
    echo 'usage: ltp.sh LTP_ROOT SUITES PATTERN RESULT_DIRECTORY' >&2
    exit 125
fi
ltp_root=$1
suites=$2
pattern=$3
result_dir=$4
case "$ltp_root" in /*) ;; *) echo 'LTP root must be absolute' >&2; exit 125;; esac
case "$result_dir" in /*) ;; *) echo 'result directory must be absolute' >&2; exit 125;; esac
case "$suites" in
    ''|,*|*,|*,,*|*[!a-zA-Z0-9_,.-]*) echo 'invalid LTP suite names' >&2; exit 125;;
esac
case "$ltp_root$result_dir$pattern" in
    *[[:space:]]*) echo 'upstream runltp requires paths and patterns without whitespace' >&2; exit 125;;
esac
case "$pattern" in -*) echo 'pattern must not begin with an option' >&2; exit 125;; esac
if [ ! -r "$ltp_root/runltp" ] || [ ! -x "$ltp_root/bin/ltp-pan" ]; then
    echo 'original runltp/ltp-pan is missing' >&2
    exit 125
fi
saved_ifs=$IFS
IFS=,
for suite in $suites; do
    if [ ! -f "$ltp_root/runtest/$suite" ] || [ ! -r "$ltp_root/runtest/$suite" ]; then
        echo "missing original LTP suite: $suite" >&2
        exit 125
    fi
done
IFS=$saved_ifs
mkdir "$result_dir" || exit 125
cd "$ltp_root" || exit 125
export LTPROOT=$ltp_root
export PATH="$LTPROOT/testcases/bin:$LTPROOT/bin:$PATH"
# 原 IDcheck 接口明确拒绝创建账户，保留其缺账户警告，避免等待串口输入。
export CREATE_ENTRIES=0
set -- -f "$suites" -p -q -l "$result_dir/results" -o "$result_dir/output" \
    -C "$result_dir/failures" -T "$result_dir/configurations"
if [ -n "$pattern" ]; then set -- "$@" -s "$pattern"; fi
echo "BOAROS-LTP ENGINE root=$LTPROOT suites=$suites pattern=$pattern"
sh "$LTPROOT/runltp" "$@" </dev/null
result=$?
missing=0
for name in results output failures configurations; do
    echo "BOAROS-LTP FILE $name"
    if [ -f "$result_dir/$name" ]; then
        cat "$result_dir/$name" || missing=1
    else
        echo "missing engine result: $name" >&2
        missing=1
    fi
done
echo "BOAROS-LTP ENGINE-EXIT root=$LTPROOT status=$result results_missing=$missing"
# 引擎错误不得改成成功；成功但缺输出也不能作为完整验证。
if [ "$result" -eq 0 ] && [ "$missing" -ne 0 ]; then exit 125; fi
exit "$result"
