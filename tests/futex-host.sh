#!/bin/sh
set -eu
mkdir -p build/host/futex
python3 - <<'PY'
from pathlib import Path
source=Path('arch/riscv/smp.c').read_text()
start=source.index('void kernel_cpu_notify(')
end=source.index('\nstatic void consume_ipi(',start)
Path('build/host/futex/notifications.c').write_text('''#include <kernel/cpu.h>
#include <assert.h>
#include <stdlib.h>
static struct kernel_cpu remote[4], *cpus[4];
static unsigned cpu_count=4;
extern long futex_test_send_ipi(uint64_t);
static long sbi_send_ipi(uint64_t hart) { return futex_test_send_ipi(hart); }
static void fail_hart(const char *stage,long error,uint64_t hart)
{ (void)stage;(void)error;(void)hart;abort(); }
void futex_test_notifications_init(void)
{
    cpus[0]=kernel_cpu_current();
    for(unsigned i=1;i<4;i++) {
        kernel_cpu_initialize(&remote[i],i,0);remote[i].logical_id=i;cpus[i]=&remote[i];
    }
}
struct kernel_cpu *futex_test_notification_target(unsigned i)
{ assert(i && i<4);return cpus[i]; }
''' + source[start:end])
PY
for shift in 12 14; do
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -DBOAROS_MAX_CPUS=4 -ffunction-sections -fdata-sections \
    -DBOAROS_PAGE_SHIFT="$shift" -Itests/host/mm -Itests/host/sync -idirafter include \
    tests/host/futex.c tests/host/mm/unavailable.c tests/host/mm/context.c \
    kernel/cpu.c kernel/raw_lock.c tests/host/sync/notifications.c build/host/futex/notifications.c kernel/sched/sync.c kernel/sched/park.c kernel/sched/futex.c kernel/sched/futex_waitv.c \
    kernel/physical_page.c mm/heap.c mm/memory_object.c mm/vma.c mm/mm.c mm/uaccess.c arch/riscv/sv39.c \
    -Wl,--gc-sections,--wrap=kernel_rwlock_write,--wrap=kernel_mm_unpin_user_page,--wrap=kernel_heap_allocate_zeroed,--wrap=kernel_wait_prepare,--wrap=kernel_wait_prepare_locked,--wrap=kernel_wait_node_bind_locked,--wrap=kernel_memory_object_prepare_page,--wrap=kernel_raw_lock_acquire,--wrap=kernel_wait_backend_notify -o "build/host/futex/direct-$shift"
timeout 30s "build/host/futex/direct-$shift"
done
