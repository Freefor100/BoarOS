#!/bin/sh
set -eu
export PATH=/usr/bin:/bin
export LD_LIBRARY_PATH=/usr/lib:/lib:/lib64
export BPFTRACE_MAX_STRLEN=64
export BPFTRACE_MAX_MAP_KEYS=128
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /tmp
mkdir -p /sys/fs/bpf /sys/kernel/tracing
mount -t bpf bpf /sys/fs/bpf
mount -t tracefs tracefs /sys/kernel/tracing
# No capability or version spoofing: these are the original executables.
bpftool version
bpftool prog show
bpftool map show
bpftool btf show
mkdir /sys/fs/bpf/maps
bpftool prog load /work/udp.bpf.o /sys/fs/bpf/policy type socket pinmaps /sys/fs/bpf/maps
bpftool prog show pinned /sys/fs/bpf/policy
bpftool map update pinned /sys/fs/bpf/maps/udp_rule key hex 00 00 00 00 value hex 01 00 00 00
bpftool map lookup pinned /sys/fs/bpf/maps/udp_rule key hex 00 00 00 00 > /tmp/rule-lookup.out
bpftool map dump pinned /sys/fs/bpf/maps/udp_rule > /tmp/rule-dump.out
cat /tmp/rule-lookup.out /tmp/rule-dump.out
grep -Eq '^[[:space:]]*"value":[[:space:]]*1[[:space:]]*$' /tmp/rule-lookup.out
grep -Eq '^[[:space:]]*"value":[[:space:]]*1[[:space:]]*$' /tmp/rule-dump.out
bpftool btf show
rm /sys/fs/bpf/policy /sys/fs/bpf/maps/*
rmdir /sys/fs/bpf/maps
if bpftool prog show pinned /sys/fs/bpf/policy > /tmp/unlinked-program.out 2>&1; then
    cat /tmp/unlinked-program.out; exit 1
fi
grep -q 'No such file or directory' /tmp/unlinked-program.out
if bpftool map show pinned /sys/fs/bpf/maps/udp_rule > /tmp/unlinked-map.out 2>&1; then
    cat /tmp/unlinked-map.out; exit 1
fi
grep -q 'No such file or directory' /tmp/unlinked-map.out
printf 'BPF BPFTOOL ELF-map-pin-get-unlink OK\n'
bpf-udp /work/udp.bpf.o
if [ "${1:-full}" = 'bpftool' ]; then
    printf 'BPF ORIGINAL TOOLS BASELINE bpftool OK\n'
    exit 0
fi
bpftrace --version
trace_case() {
    mode=$1
    output=/tmp/$mode.out
    errors=/tmp/$mode.err
    mkfifo /tmp/bpf-start /tmp/bpf-ready /tmp/bpf-control /tmp/bpf-output
    bpf-syscalls "$mode" &
    target=$!
    read -r ready < /tmp/bpf-ready
    [ "$ready" = R ]
    bpftrace -B line -q -p "$target" /work/syscalls.bt "$target" > /tmp/bpf-output 2> "$errors" &
    tracer=$!
    exec 3< /tmp/bpf-output
    : > "$output"
    begin=0
    # Upstream consumes BEGIN's real ringbuf record only after every tracepoint
    # attaches. The target remains blocked on its FIFO until that record arrives.
    while IFS= read -r line <&3; do
        printf '%s\n' "$line" >> "$output"
        if [ "$line" = 'BPFTRACE COMPILED BEGIN' ]; then begin=1; break; fi
    done
    if [ "$begin" -ne 1 ]; then cat "$output" "$errors"; return 1; fi
    printf G > /tmp/bpf-start
    if [ "$mode" = interrupt ]; then
        # This acknowledgement is sent after all 64 syscall exits have occurred.
        read -r ready < /tmp/bpf-ready
        [ "$ready" = R ]
        # SIGINT may stop upstream output polling immediately. Acknowledge the
        # actual 64 ringbuf records before interrupting the live tracer.
        seen=0
        while [ "$seen" -lt 64 ] && IFS= read -r line <&3; do
            printf '%s\n' "$line" >> "$output"
            case "$line" in 'BPFTRACE syscall pid='*) seen=$((seen+1));; esac
        done
        [ "$seen" -eq 64 ]
        kill -INT "$tracer"
        printf X > /tmp/bpf-control
    fi
    wait "$target"
    # The original -p monitor uses kill(pid, 0). Do not fork a drain process
    # after reaping the target: immediate PID reuse could make that process
    # keep the monitor alive while waiting for the tracer to close this FIFO.
    while IFS= read -r line <&3; do
        printf '%s\n' "$line" >> "$output"
    done
    exec 3<&-
    wait "$tracer"
    cat "$output" "$errors"
    grep -q 'BPFTRACE CLEAN END' "$output"
    [ "$(grep -c 'BPFTRACE syscall pid=' "$output")" -eq 64 ]
    grep -Eq '@entered\[[0-9]+, [0-9]+\]: 64' "$output"
    grep -Eq '@completed\[[0-9]+, [0-9]+\]: 64' "$output"
    grep -Eq '@latency_ns\[[0-9]+, [0-9]+\]: [1-9][0-9]*' "$output"
    if grep -q 'BPFTRACE DELETE FAILED' "$output"; then return 1; fi
    rm /tmp/bpf-start /tmp/bpf-ready /tmp/bpf-control /tmp/bpf-output
}
trace_case normal
trace_case interrupt
printf 'BPF BPFTRACE PID-TID-count-latency-ringbuf-normal-SIGINT OK\n'
printf 'BPF ORIGINAL TOOLS BASELINE full OK\n'
