#include <kernel/cpu_config.h>
#if BOAROS_MAX_CPUS > 1
    .section .text.boot, "ax", @progbits
    .globl la_secondary_entry
la_secondary_entry:
    /* Firmware parks APs in their mailbox loop; only BSP clears BSS. */
    li.d $t0, 0x9000000000000011
    csrwr $t0, 0x180
    li.d $t0, 0x8000000000000001
    csrwr $t0, 0x181
    li.d $t0, 0x11
    csrwr $t0, 0x182
    li.d $t0, 0x10
    csrwr $t0, 0
    csrwr $zero, 2
    csrwr $zero, 4
    la.abs $t0, .Lsecondary_high
    jr $t0
.Lsecondary_high:
    csrwr $zero, 0x182
    la.abs $sp, la_secondary_stack_top
    la.abs $t0, la_secondary_thread
    dbar 0
    ld.d $tp, $t0, 0
    bl la_smp_secondary
.Lstopped:
    idle 0
    b .Lstopped
    .section .bss, "aw", @nobits
    .balign 16384
    .globl la_secondary_stack
la_secondary_stack:
    .skip 16384
    .globl la_secondary_stack_top
la_secondary_stack_top:
#endif
