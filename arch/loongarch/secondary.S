#include <kernel/cpu_config.h>
#include <arch/loongarch/smp_boot.h>
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
    /* Firmware consumes MBUF0 as entry; MBUF1 keeps this target's owner. */
    li.w $t0, 0x1028
    iocsrrd.d $a0, $t0
    dbar 0
    ld.d $tp, $a0, LA_SMP_BOOT_THREAD
    ld.d $sp, $a0, LA_SMP_BOOT_STACK_HIGH
    bl la_smp_secondary
.Lstopped:
    idle 0
    b .Lstopped
#endif
