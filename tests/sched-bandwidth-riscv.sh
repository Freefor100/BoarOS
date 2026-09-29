#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "$root/build/riscv/sched-bandwidth.XXXXXX")
trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work"; else echo "scheduler test artifacts: $work" >&2; fi' EXIT
compiler="$root/build/riscv/musl-root/bin/musl-gcc"
flags=
if "$compiler" -fno-link-libatomic -E -x c /dev/null >/dev/null 2>&1; then flags=-fno-link-libatomic; fi
"$compiler" $flags -static -O2 -Wall -Wextra -Werror "$root/tests/userland/sched-bandwidth.c" -o "$work/init"
truncate -s 32M "$work/root.img"
mkfs.ext4 -q -F "$work/root.img"
debugfs -w -R "write $work/init /init" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /init mode 0100755' "$work/root.img" >/dev/null 2>&1
if ! timeout -k 2s "${SCHED_TIMEOUT:-120s}" "${QEMU_RISCV64:-qemu-system-riscv64}" \
    -machine virt -bios default -kernel "${KERNEL_RV:-$root/kernel-rv}" \
    -m 512M -smp 1 -nographic -no-reboot \
    -object rng-random,id=entropy,filename=/dev/urandom \
    -device virtio-rng-device,rng=entropy,bus=virtio-mmio-bus.7 \
    -drive file="$work/root.img",if=none,format=raw,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$work/output" 2>&1; then
    tail -100 "$work/output" >&2; exit 1
fi
for marker in 'sched proc stat ok' 'sched budget configuration ok' 'sched short budget ok' 'sched RR preemption ok' 'sched RR quota boundary ok' 'sched bandwidth checks ok'; do
    test "$(grep -c "BoarOS: $marker" "$work/output")" -eq 1 || { cat "$work/output"; exit 1; }
done
grep -E '^BoarOS: sched |^BoarOS: PID 1 exited' "$work/output"
grep -qE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$work/output"
python3 "$root/tests/check-stack-report.py" "$work/output"
