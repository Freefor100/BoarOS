#!/bin/sh
# The shell harness also calls release for a supplied block partition. Only
# a device acquired from the loop pool needs a loop detach.
if [ "$#" -eq 2 ] && [ "$1" = release ] && [ "$2" = "${LTP_DEV:-}" ] && [ -b "$2" ]; then
    if number=$(/boaros-tools/stat -c '%t:%T' -- "$2") &&
       mounts=$(cat /proc/self/mountinfo) && [ -n "$mounts" ]; then
        major=${number%:*}
        minor=${number#*:}
        device="$((0x$major)):$((0x$minor))"
        if printf '%s\n' "$mounts" | awk -v device="$device" '$3 == device { mounted = 1 } END { exit mounted }'; then
            exit 0
        fi
    fi
fi
exec "$LTPROOT/testcases/bin/tst_device" "$@"
