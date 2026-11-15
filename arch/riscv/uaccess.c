#include <arch/riscv/uaccess.h>
#include <arch/riscv/trap.h>
#include <arch/riscv/sv39.h>

struct uaccess_recovery { uintptr_t instruction, recovery; };
extern const struct uaccess_recovery riscv_uaccess_fixups_start[],riscv_uaccess_fixups_end[];

int riscv_uaccess_fault(struct riscv_trap_frame *frame)
{
    if (!(frame->sstatus & (1UL<<18)) || frame->stval>=RISCV_SV39_USER_LIMIT ||
        (frame->scause!=RISCV_SCAUSE_LOAD_PAGE_FAULT &&
         frame->scause!=RISCV_SCAUSE_STORE_PAGE_FAULT &&
         frame->scause!=5 && frame->scause!=7)) return 0;
    for (const struct uaccess_recovery *entry=riscv_uaccess_fixups_start;
         entry<riscv_uaccess_fixups_end;entry++) {
        if (frame->sepc!=entry->instruction) continue;
        frame->t5=frame->scause;
        frame->sepc=entry->recovery;
        frame->sstatus&=~(1UL<<18);
        return 1;
    }
    return 0;
}
