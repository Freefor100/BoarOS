#!/musl/busybox sh
set -e
BB=/musl/busybox
$BB mkdir -p /bin /lib /tmp /dev/misc /dev/block /dev/shm /proc
$BB --install -s /bin
$BB ln -s /musl/busybox /bin/busybox
$BB ln -s /glibc/lib/ld-linux-riscv64-lp64d.so.1 /lib/ld-linux-riscv64-lp64d.so.1
$BB ln -s /musl/lib/libc.so /lib/ld-musl-riscv64.so.1
$BB ln -s /musl/lib/libc.so /lib/ld-musl-riscv64-sf.so.1
$BB mknod -m 666 /dev/null c 1 3
$BB mknod -m 666 /dev/zero c 1 5
$BB mknod -m 600 /dev/console c 5 1
$BB mknod -m 600 /dev/ttyS0 c 4 64
$BB mknod -m 666 /dev/tty c 5 0
$BB mknod -m 666 /dev/random c 1 8
$BB mknod -m 666 /dev/urandom c 1 9
$BB mknod -m 600 /dev/rtc0 c 10 135
$BB ln -s /dev/rtc0 /dev/rtc
$BB ln -s /dev/rtc0 /dev/misc/rtc
$BB mknod -m 600 /dev/block/252:0 b 252 0
exec </dev/console >/dev/console 2>&1
$BB mount -t proc proc /proc
$BB mount -t tmpfs -o mode=1777 tmpfs /dev/shm
$BB chmod 1777 /tmp
# 发布镜像将此脚本存成 0644；补执行权限，不改测例内容或判断。
$BB chmod +x /glibc/basic/run-all.sh /musl/basic/run-all.sh
export HOME=/ TERM=vt100
groups='basic busybox cyclictest iozone iperf libcbench libctest lmbench lua netperf ltp'
if [ -f /boaros-eval-groups ]; then groups=$($BB cat /boaros-eval-groups); fi
for group in $groups; do
    for libc in glibc musl; do
        (
            cd /$libc
            export PATH=/bin:/$libc:/$libc/ltp/testcases/bin:.
            export LD_LIBRARY_PATH=/$libc/lib
            if [ "$group" = lmbench ]; then
                $BB mkdir -p /code/lmbench_src/bin/build
                $BB ln -sf /$libc/lmbench_all /code/lmbench_src/bin/build/lmbench_all
            fi
            echo "BOAROS-EVAL ENTER $group-$libc"
            set +e
            /$libc/busybox sh ./${group}_testcode.sh
            result=$?
            echo "BOAROS-EVAL EXIT $group-$libc status=$result"
        )
    done
done
echo 'BOAROS-EVAL COMPLETE'
