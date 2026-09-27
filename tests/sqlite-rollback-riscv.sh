#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
program=${SQLITE_ROLLBACK_RV:-"$root/build/riscv/tests/user/sqlite-rollback-rv"}
cli_init=${SQLITE_CLI_INIT_RV:-"$root/build/riscv/tests/user/sqlite-cli-init-rv"}
cli_static=${SQLITE_CLI_STATIC_RV:-"$root/build/riscv/tests/user/sqlite3-static-rv"}
cli_dynamic=${SQLITE_CLI_DYNAMIC_RV:-"$root/build/riscv/tests/user/sqlite3-dynamic-rv"}
ldso=${MUSL_LDSO:-"$root/build/riscv/musl-root/lib/ld-musl-riscv64.so.1"}
kernel=${KERNEL_RV:-"$root/kernel-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
work=$(mktemp -d "$root/build/riscv/sqlite-run.XXXXXX")
trap 'rc=$?; if [ "$rc" -eq 0 ]; then rm -rf "$work"; else echo "SQLite artifacts retained: $work" >&2; fi' EXIT
truncate -s 64M "$work/root.img"
mkfs.ext4 -q -F "$work/root.img"
debugfs -w -R "write $program /init" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /init mode 0100755' "$work/root.img" >/dev/null 2>&1
if ! timeout -k 2s 60s "$qemu" -machine virt -bios default -kernel "$kernel" \
    -m 512M -smp 1 -nographic -no-reboot \
    -drive file="$work/root.img",if=none,format=raw,readonly=off,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$work/boot.log" 2>&1; then
    tail -80 "$work/boot.log" >&2
    exit 1
fi
if ! grep -qxF 'BoarOS: SQLite rollback smoke passed' "$work/boot.log"; then
    tail -80 "$work/boot.log" >&2
    exit 1
fi
if ! grep -qE '^BoarOS: PID 1 exited status=0x2a pages=0x[1-9a-f][0-9a-f]* heap-live=0x0; shutting down$' "$work/boot.log"; then
    tail -80 "$work/boot.log" >&2
    exit 1
fi
debugfs -w -R 'rm /init' "$work/root.img" >/dev/null 2>&1
debugfs -w -R "write $cli_init /init" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /init mode 0100755' "$work/root.img" >/dev/null 2>&1
debugfs -w -R "write $cli_static /sqlite3-static" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /sqlite3-static mode 0100755' "$work/root.img" >/dev/null 2>&1
debugfs -w -R "write $cli_dynamic /sqlite3-dynamic" "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /sqlite3-dynamic mode 0100755' "$work/root.img" >/dev/null 2>&1
debugfs -w -R 'mkdir /lib' "$work/root.img" >/dev/null 2>&1
debugfs -w -R "write $ldso /lib/ld-musl-riscv64.so.1" "$work/root.img" >/dev/null 2>&1
if ! timeout -k 2s 60s "$qemu" -machine virt -bios default -kernel "$kernel" \
    -m 512M -smp 1 -nographic -no-reboot \
    -drive file="$work/root.img",if=none,format=raw,readonly=off,id=root \
    -device virtio-blk-device,drive=root,bus=virtio-mmio-bus.0 \
    </dev/null >"$work/cli.log" 2>&1; then
    tail -80 "$work/cli.log" >&2
    exit 1
fi
if ! grep -qxF 'BoarOS: SQLite CLI static and dynamic passed' "$work/cli.log"; then
    tail -80 "$work/cli.log" >&2
    exit 1
fi
echo 'SQLite rollback smoke passed'
