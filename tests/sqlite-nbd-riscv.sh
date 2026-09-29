#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
program=${SQLITE_ROLLBACK_RV:-"$root/build/riscv/tests/user/sqlite-rollback-rv"}
kernel=${KERNEL_RV:-"$root/kernel-rv"}
server=${NBD_FAULT_SERVER:-"$root/build/host/nbd-fault"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
work=$(mktemp -d "$root/build/riscv/sqlite-nbd-run.XXXXXX")
backend_pid=
trap 'rc=$?; if [ -n "$backend_pid" ]; then kill "$backend_pid" 2>/dev/null || :; wait "$backend_pid" 2>/dev/null || :; fi; if [ "$rc" -eq 0 ]; then rm -rf "$work"; else echo "SQLite NBD artifacts retained: $work" >&2; fi' EXIT
truncate -s 64M "$work/root.img"
mkfs.ext4 -q -F "$work/root.img"
debugfs -w -R "write $program /init" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /init mode 0100755' "$work/root.img" >/dev/null 2>&1
"$server" "$work/root.img" "$work/root.sock" >"$work/nbd.log" 2>&1 &
backend_pid=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$work/root.sock" ] && break
    sleep 0.1
done
[ -S "$work/root.sock" ]
if ! timeout -k 2s 90s "$qemu" -machine virt -bios default -kernel "$kernel" \
    -m 512M -smp 1 -nographic -no-reboot \
    -object rng-random,id=entropy,filename=/dev/urandom \
    -device virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7 \
    -drive file="nbd:unix:$work/root.sock",if=none,format=raw,readonly=off,id=root,cache=writeback \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$work/boot.log" 2>&1; then
    tail -80 "$work/boot.log" >&2
    tail -40 "$work/nbd.log" >&2
    exit 1
fi
if ! grep -qxF 'BoarOS: SQLite rollback smoke passed' "$work/boot.log" ||
   ! grep -qE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$work/boot.log"; then
    tail -80 "$work/boot.log" >&2
    exit 1
fi
wait "$backend_pid"
backend_pid=
grep -q 'type=WRITE' "$work/nbd.log"
grep -q 'type=FLUSH' "$work/nbd.log"
debugfs -R 'stat /boaros.db' "$work/root.img" 2>/dev/null | grep -q 'Type: regular'
printf 'SQLite rollback via NBD passed (%s writes, %s flushes)\n' \
    "$(grep -c 'type=WRITE' "$work/nbd.log")" \
    "$(grep -c 'type=FLUSH' "$work/nbd.log")"
