#!/musl/busybox sh
set -e
BB=/musl/busybox
$BB mkdir -p /bin /lib /lib64 /usr/lib64 /tmp /dev/misc /dev/block /dev/shm /proc
$BB --install -s /bin
$BB ln -s /musl/busybox /bin/busybox
# BOAROS_LOADER_LINKS
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
# BOAROS_CASE_PAYLOAD
# BOAROS_RUNTIME_PAYLOAD
groups=${BOAROS_EVAL_GROUPS:-'basic busybox cyclictest iozone iperf libcbench libctest lmbench lua netperf ltp'}
export BOAROS_LTP_CASE_TIMEOUT=${BOAROS_LTP_CASE_TIMEOUT:-300}
for group in $groups; do
    for libc in glibc musl; do
        (
            cd /$libc
            export PATH=/bin:/$libc:/$libc/ltp/testcases/bin:.
            export LD_LIBRARY_PATH=/$libc/lib
            # 按运行时环境统一接入Linux接口，不识别程序名称或组别。
            if [ "$libc" = musl ] && [ -n "${BOAROS_LINUX_SCHED_PRELOAD:-}" ]; then
                export LD_PRELOAD=$BOAROS_LINUX_SCHED_PRELOAD
            else
                unset LD_PRELOAD
            fi
            if [ "$group" = lmbench ]; then
                $BB mkdir -p /code/lmbench_src/bin/build
                $BB ln -sf /$libc/lmbench_all /code/lmbench_src/bin/build/lmbench_all
            fi
            echo "BOAROS-EVAL ENTER $group-$libc"
            set +e
            if [ "$group" = ltp ]; then
                export LTPROOT=/$libc/ltp
                # LTP 的 test.sh 必须先于 libc 根目录的同名 Lua 驱动脚本。
                export PATH=/bin:$LTPROOT/testcases/bin:/$libc:.
                export BOAROS_CASE_SHELL=/$libc/busybox
                # 仅在临时副本中接入逐项监督，原循环、参数、标记和判分行均保留。
                if ! $BB sh /tmp/boaros-ltp-hook.sh ./ltp_testcode.sh /tmp/boaros-ltp-$libc.sh; then
                    result=125
                else
                    /$libc/busybox sh /tmp/boaros-ltp-$libc.sh
                    result=$?
                fi
            elif [ "$group" = libctest ]; then
                export BOAROS_CASE_SHELL=/$libc/busybox
                if ! $BB sh /tmp/boaros-libctest-hook.sh ./libctest_testcode.sh /tmp/boaros-libctest-$libc.sh; then
                    result=125
                else
                    /$libc/busybox sh /tmp/boaros-libctest-$libc.sh
                    result=$?
                fi
            else
                /$libc/busybox sh ./${group}_testcode.sh
                result=$?
            fi
            echo "BOAROS-EVAL EXIT $group-$libc status=$result"
        )
    done
done
echo 'BOAROS-EVAL COMPLETE'
