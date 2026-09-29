#!/bin/sh
# The fixed, unmodified BusyBox runs every filesystem operation below.
set -eu
/busybox mkdir /tmpfs-check
/busybox mount -n -t tmpfs -o size=1048576,nr_inodes=64 none /tmpfs-check
/busybox test "$(/busybox stat -f -c %t /tmpfs-check)" = 1021994
cd /tmpfs-check
/busybox pwd
printf 'tmpfs payload\n' > before
/busybox cp before copied
/busybox cmp before copied
/busybox mv copied moved
/busybox test ! -e copied
/busybox ln moved hard
/busybox stat -c '%h' hard
/busybox ln -s hard symbolic
/busybox cat symbolic
/busybox rm moved
/busybox stat -c '%h' hard
# An open fd must keep the inode alive after its final hard link disappears.
exec 3<hard
/busybox rm hard
/busybox test ! -e hard
/busybox test ! -e symbolic
/busybox cat <&3
exec 3<&-
/busybox rm symbolic before
/busybox dd if=/dev/zero of=blocks bs=512 count=2 2>/dev/null
/busybox wc -c < blocks
/busybox cp blocks copy-blocks
/busybox cmp blocks copy-blocks
/busybox rm blocks copy-blocks
/busybox mkdir child
cd child
/busybox pwd
cd ..
/busybox mv child renamed
cd renamed
/busybox pwd
printf 'nested write\n' > leaf
/busybox cat leaf
cd /
/busybox umount -n /tmpfs-check
/busybox test ! -e /tmpfs-check/renamed
/busybox rmdir /tmpfs-check
printf 'tmpfs-filesystem-ok\n'
