#!/bin/sh
set -eu
dmesg -r > /tmp/log-before-clear
grep -Eq 'BoarOS:|Linux version' /tmp/log-before-clear
echo 'ENV raw log OK'
dmesg -n 1
dmesg -n 8
dmesg -c > /tmp/log-clear
test -s /tmp/log-clear
dmesg -r > /tmp/log-after-clear
test ! -s /tmp/log-after-clear
echo 'ENV log clear OK'
hwclock -r > /tmp/rtc-time
grep -Eq '[0-9]{4}.*seconds' /tmp/rtc-time
echo 'ENV RTC content OK'
set -- $(stat -f -c '%b %f %a %S %T' /)
blocks=$1
free=$2
available=$3
unit=$4
test "$5" = ext2/ext3
set -- $(df -k / | awk '$NF == "/" { print $2, $3, $4 }')
test "$#" -eq 3
test "$1" -eq "$((blocks * unit / 1024))"
test "$2" -eq "$(((blocks - free) * unit / 1024))"
test "$3" -eq "$((available * unit / 1024))"
echo 'ENVIRONMENT CONTENT PASS'
