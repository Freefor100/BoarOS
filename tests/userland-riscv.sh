#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KERNEL_RV:-"$project_root/kernel-rv"}
program=${REAL_USERLAND_RV:-"$project_root/build/riscv/tests/user/real-userland-rv"}
pthread_program=${PTHREAD_USERLAND_RV:-"$project_root/build/riscv/tests/user/pthread-userland-rv"}
tls_dso=${PTHREAD_TLS_DSO_RV:-"$project_root/build/riscv/tests/user/libboaros-tls.so"}
musl_ldso=${MUSL_LDSO:-"$project_root/build/riscv/musl-root/lib/ld-musl-riscv64.so.1"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
memory=${QEMU_MEMORY:-512M}
mkdir -p "$project_root/build/riscv"
work_dir=$(mktemp -d "$project_root/build/riscv/userland-run.XXXXXX")
static_output="$work_dir/static-userland.log"
pthread_output="$work_dir/pthread-userland.log"
static_disk="$work_dir/static-root.img"
pthread_disk="$work_dir/pthread-root.img"
data="$work_dir/data"

trap 'result=$?; if [ "$result" -eq 0 ]; then rm -rf "$work_dir"; else echo "userland artifacts retained: $work_dir" >&2; fi' EXIT
trap 'exit 1' HUP INT TERM

for tool in truncate mkfs.ext4 debugfs awk; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing userland test tool: $tool" >&2
        exit 1
    fi
done
if [ ! -f "$kernel" ]; then
    echo "missing production kernel: $kernel" >&2
    exit 1
fi
if [ ! -f "$program" ]; then
    echo "missing real userland program: $program" >&2
    exit 1
fi
for artifact in "$pthread_program" "$tls_dso" "$musl_ldso"; do
    if [ ! -f "$artifact" ]; then
        echo "missing pthread userland artifact: $artifact" >&2
        exit 1
    fi
done

awk 'BEGIN { for (i = 0; i < 9000; i++) printf "%c", 65 + (i % 26) }' \
    >"$data"

truncate -s 32M "$static_disk"
mkfs.ext4 -q -F "$static_disk"
debugfs -w -R "write $program /init" "$static_disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init mode 0100755" "$static_disk" \
    >/dev/null 2>&1
debugfs -w -R "write $data /data" "$static_disk" >/dev/null 2>&1

# The userland program blocks reading stdin after the clock and sleep
# checks, so the harness feeds one line into the serial console.
if ! { sleep 4; printf 'go\n'; } | timeout -k 2s 15s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m "$memory" \
    -smp 1 \
    -nographic \
    -no-reboot \
    -drive file="$static_disk",if=none,format=raw,readonly=off,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    >"$static_output" 2>&1; then
    tail -n 120 "$static_output" >&2
    echo "production kernel failed to run the real userland program" >&2
    exit 1
fi

for marker in \
    'BoarOS: real userland stdio ok' \
    'BoarOS: real userland file checks ok' \
    'BoarOS: real userland fs rw checks ok' \
    'BoarOS: real userland clock checks ok' \
    'BoarOS: real userland sleep checks ok' \
    'BoarOS: real userland console input ok' \
    'BoarOS: real userland fp checks ok' \
    'BoarOS: real userland signal checks ok' \
    'BoarOS: real userland pipe checks ok' \
    'BoarOS: real userland poll/select checks ok' \
    'BoarOS: real userland epoll checks ok'; do
    if [ "$(grep -cxF "$marker" "$static_output" || true)" -ne 1 ]; then
        tail -n 120 "$static_output" >&2
        echo "real userland marker missing or duplicated: $marker" >&2
        exit 1
    fi
done
if [ "$(grep -cE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$static_output" || true)" -ne 1 ]; then
    tail -n 120 "$static_output" >&2
    echo "real userland program did not exit cleanly with all resources reaped" >&2
    exit 1
fi
if grep -qE 'BoarOS: (root boot error|scheduler startup/idle error|fatal trap|SBI shutdown failed)' "$static_output"; then
    tail -n 120 "$static_output" >&2
    echo "real userland boot reported an error" >&2
    exit 1
fi

persisted_content=$(debugfs -R "cat /persist.txt" "$static_disk" 2>/dev/null || true)
if [ "$persisted_content" != "BoarOS-ext4-persisted-data" ]; then
    echo "persisted content mismatch on static root: expected 'BoarOS-ext4-persisted-data', got '$persisted_content'" >&2
    exit 1
fi

truncate -s 32M "$pthread_disk"
mkfs.ext4 -q -F "$pthread_disk"
debugfs -w -R "write $pthread_program /init" "$pthread_disk" >/dev/null 2>&1
debugfs -w -R "set_inode_field /init mode 0100755" "$pthread_disk" \
    >/dev/null 2>&1
debugfs -w -R "mkdir /lib" "$pthread_disk" >/dev/null 2>&1
debugfs -w -R "write $musl_ldso /lib/ld-musl-riscv64.so.1" "$pthread_disk" \
    >/dev/null 2>&1
debugfs -w -R "set_inode_field /lib/ld-musl-riscv64.so.1 mode 0100755" \
    "$pthread_disk" >/dev/null 2>&1
debugfs -w -R "write $tls_dso /lib/libboaros-tls.so" "$pthread_disk" \
    >/dev/null 2>&1

if ! timeout -k 2s 20s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m "$memory" \
    -smp 1 \
    -nographic \
    -no-reboot \
    -drive file="$pthread_disk",if=none,format=raw,readonly=on,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$pthread_output" 2>&1; then
    tail -n 160 "$pthread_output" >&2
    echo "production kernel failed to run the pthread userland program" >&2
    exit 1
fi

for marker in \
    'BoarOS: real pthread TLS checks ok' \
    'BoarOS: real pthread synchronization checks ok' \
    'BoarOS: real pthread cancellation checks ok' \
    'BoarOS: real pthread dlopen TLS checks ok' \
    'BoarOS: real pthread shared fd checks ok' \
    'BoarOS: real pthread lifecycle checks ok' \
    'BoarOS: real pthread futex ABI checks ok'; do
    if [ "$(grep -cxF "$marker" "$pthread_output" || true)" -ne 1 ]; then
        tail -n 160 "$pthread_output" >&2
        echo "pthread userland marker missing or duplicated: $marker" >&2
        exit 1
    fi
done
if [ "$(grep -cE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$pthread_output" || true)" -ne 1 ]; then
    tail -n 160 "$pthread_output" >&2
    echo "pthread userland program did not exit cleanly with all resources reaped" >&2
    exit 1
fi
if grep -qE 'BoarOS: (root boot error|scheduler startup/idle error|fatal trap|SBI shutdown failed)' "$pthread_output"; then
    tail -n 160 "$pthread_output" >&2
    echo "pthread userland boot reported an error" >&2
    exit 1
fi

echo "RISC-V static and pthread userland programs ran on the production kernel"
