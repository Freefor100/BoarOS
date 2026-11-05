#!/bin/sh
# Run with the unchanged pinned BusyBox ash and applets in the suite fixture.
set -eu
mkdir /fs-check
cd /fs-check
/busybox pwd
printf 'durable contents\n' > before
/busybox touch -t 202001020304.05 before
/busybox mv before after
/busybox cat after
/busybox test ! -e before
/busybox test -f after
/busybox stat -c '%Y' after
mkdir child
cd child
/busybox pwd
cd ..
/busybox mv child renamed
cd renamed
/busybox pwd
/busybox touch created
/busybox test -f created
printf 'filesystem-ok\n'
