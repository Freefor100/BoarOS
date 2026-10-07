#!/musl/busybox sh
# 内核携带的共享对象由guest原BusyBox发布，宿主不修改运行盘。
set -e
BB=/musl/busybox
if [ "$#" != 2 ]; then
    echo 'BOAROS-RUNTIME invalid payload arguments' >&2
    exit 125
fi
target=$1
payload=$2
if ! $BB printf '%b' "$payload" | $BB gzip -dc > "$target.tmp"; then
    $BB rm -f "$target.tmp"
    echo 'BOAROS-RUNTIME payload publication failed' >&2
    exit 125
fi
$BB chmod 444 "$target.tmp"
$BB mv -f "$target.tmp" "$target"
echo 'BOAROS-RUNTIME linux-sched ready'
