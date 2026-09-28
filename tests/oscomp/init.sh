#!/musl/busybox sh
set -e
BB=/musl/busybox
$BB mkdir -p /bin /lib /tmp /dev /proc
$BB --install -s /bin
$BB ln -s /musl/busybox /bin/busybox
$BB ln -s /glibc/lib/ld-linux-riscv64-lp64d.so.1 /lib/ld-linux-riscv64-lp64d.so.1
$BB ln -s /musl/lib/libc.so /lib/ld-musl-riscv64.so.1
$BB ln -s /musl/lib/libc.so /lib/ld-musl-riscv64-sf.so.1
$BB mknod -m 666 /dev/null c 1 3
$BB mknod -m 666 /dev/zero c 1 5
$BB mknod -m 600 /dev/console c 5 1
exec </dev/console >/dev/console 2>&1
$BB mount -t proc proc /proc
export HOME=/ TERM=vt100
for group in basic busybox cyclictest iozone iperf libcbench libctest lmbench ltp lua netperf; do
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
