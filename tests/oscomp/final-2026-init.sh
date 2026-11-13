#!/glibc/busybox sh
set -e
BB=/glibc/busybox
$BB mkdir -p /proc /sys /dev/shm /dev/pts /tmp
$BB mount -t proc proc /proc
$BB mount -t sysfs sysfs /sys
$BB mount -t tmpfs -o mode=1777 tmpfs /dev/shm
$BB mount -t devpts devpts /dev/pts
if [ ! -e /dev/vda ]; then $BB mknod /dev/vda b 252 0; fi
$BB chmod 1777 /tmp
$BB ifconfig lo 127.0.0.1 up
export PATH=/root/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/glibc
export HOME=/root TERM=vt100
cd /glibc
for group in $BOAROS_EVAL_GROUPS; do
    echo "BOAROS-EVAL ENTER $group-glibc"
    set +e
    case "$group" in
        cagent) /bin/bash ./cagent_testcode.sh ;;
        buildstorm) /bin/sh ./buildstorm_testcode.sh ;;
        *) exit 125 ;;
    esac
    result=$?
    set -e
    echo "BOAROS-EVAL EXIT $group-glibc status=$result"
done
$BB sync
echo 'BOAROS-EVAL COMPLETE'
