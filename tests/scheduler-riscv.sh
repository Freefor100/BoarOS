#!/bin/sh

set -eu

project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${SCHEDULER_BOOT_TEST_KERNEL_RV:-"$project_root/build/riscv/tests/kernel-scheduler-boot-rv"}
qemu=${QEMU_RISCV64:-qemu-system-riscv64}
objdump=${OBJDUMP_RV:-riscv64-unknown-elf-objdump}
scheduler_object=${SCHEDULER_OBJECT_RV:-"$project_root/build/riscv/kernel/sched/core.o"}
output_dir=$(mktemp -d)
output="$output_dir/scheduler.log"
hot_disassembly="$output_dir/scheduler-hot.dis"

trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

if [ ! -f "$kernel" ]; then
    echo "missing scheduler boot kernel: $kernel" >&2
    exit 1
fi
if [ ! -f "$scheduler_object" ]; then
    echo "missing scheduler object: $scheduler_object" >&2
    exit 1
fi

for symbol in \
    kernel_scheduler_on_tick \
    activate_thread_address_space \
    validate_current \
    validate_queues
do
    "$objdump" -dr --disassemble="$symbol" "$scheduler_object" \
        >>"$hot_disassembly"
    if ! grep -q "<$symbol>:" "$hot_disassembly"; then
        cat "$hot_disassembly" >&2
        echo "missing scheduler hot-path symbol: $symbol" >&2
        exit 1
    fi
done

validate_thread_symbol=$(
    "$objdump" -t "$scheduler_object" |
        awk '$NF ~ /^validate_thread(\.part\.[0-9]+)?$/ { print $NF; exit }'
)
if [ -z "$validate_thread_symbol" ]; then
    echo "missing scheduler hot-path symbol: validate_thread" >&2
    exit 1
fi
"$objdump" -dr --disassemble="$validate_thread_symbol" "$scheduler_object" \
    >>"$hot_disassembly"
if ! grep -q "<$validate_thread_symbol>:" "$hot_disassembly"; then
    cat "$hot_disassembly" >&2
    echo "missing scheduler hot-path symbol: $validate_thread_symbol" >&2
    exit 1
fi

# 地址空间活动集合属于切换交接；该入口不取得/归还引用或执行映射业务。
# 其余MM/PID/页生命周期仍禁止进入tick/挑选路径。
if sed '/R_RISCV_CALL.*kernel_mm_activate[[:space:]]*$/d' "$hot_disassembly" |
    grep -Eq 'R_RISCV_CALL(_PLT)?[[:space:]]+(kernel_mm_|riscv_kernel_mm_|kernel_pid_|physical_page_|kernel_files_|kernel_fs_context_|kernel_exec_)'; then
    cat "$hot_disassembly" >&2
    echo "scheduler tick path performs MM, PID, or physical-page lifecycle work" >&2
    exit 1
fi

mm_object=$(dirname "$scheduler_object")/../../mm/mm.o
"$objdump" -dr --disassemble=kernel_mm_activate "$mm_object" >"$output_dir/mm-activate.dis"
if ! grep -q '<kernel_mm_activate>:' "$output_dir/mm-activate.dis" ||
   grep -Eq 'R_RISCV_CALL(_PLT)?[[:space:]]+(kernel_mm_(create|acquire|release|fork|pin|unpin|resolve_user)|physical_page_(allocate|acquire|release)|kernel_heap_|kernel_pid_|kernel_files_|kernel_fs_context_|kernel_exec_)' "$output_dir/mm-activate.dis"; then
    cat "$output_dir/mm-activate.dis" >&2
    echo "MM activation changes lifecycle owners or enters business work" >&2
    exit 1
fi

if ! timeout -k 2s 10s "$qemu" \
    -machine virt \
    -bios default \
    -kernel "$kernel" \
    -m 512M \
    -smp 1 \
    -nographic \
    -no-reboot </dev/null >"$output" 2>&1; then
    tail -n 120 "$output" >&2
    echo "QEMU failed during scheduler preemption test" >&2
    exit 1
fi

if [ "$(grep -cxF 'BoarOS: scheduler preemption order=0x1212 workers=0x3 reaped=0x2 failures=0x0' "$output" || true)" -ne 1 ]; then
    tail -n 120 "$output" >&2
    echo "scheduler did not complete timer-only A-B-A preemption" >&2
    exit 1
fi
if grep -qE 'BoarOS: (fatal trap|timer error|scheduler error|scheduler preemption timeout)' \
    "$output"; then
    tail -n 120 "$output" >&2
    echo "scheduler preemption test reported a fatal path" >&2
    exit 1
fi

echo "RISC-V timer-only scheduler preemption passed"
