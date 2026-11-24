#include <arch/loongarch/boot_map.h>

    .macro cached_alias address, scratch, symbol
    la.abs \address, \symbol
    bstrpick.d \address, \address, LA_DIRECT_BITS - 1, 0
    li.d \scratch, LA_BOOT_CACHED_BASE
    or \address, \address, \scratch
    .endm

    .section .text.boot, "ax", @progbits
    .globl _start
_start:
    li.d $t0, 0x9000000000000011
    csrwr $t0, 0x180
    li.d $t0, 0x8000000000000001
    csrwr $t0, 0x181
    /* 切换分页时当前低地址指令仍须可取，跳到 DMW 后立即撤销。 */
    li.d $t0, 0x11
    csrwr $t0, 0x182
    li.d $t0, 0x10
    csrwr $t0, 0
    li.d $t0, 0
    csrwr $t0, 2
    csrwr $t0, 4
    cached_alias $t0, $t1, .Lmapped
    jr $t0
.Lmapped:
    csrwr $zero, 0x182
    cached_alias $t0, $t2, __bss_start
    cached_alias $t1, $t2, __bss_end
.Lzero:
    bgeu $t0, $t1, .Lready
    st.d $zero, $t0, 0
    addi.d $t0, $t0, 8
    b .Lzero
.Lready:
    cached_alias $t0, $t2, la_paged_kernel_root
    cached_alias $t1, $t2, la_paged_boot_ram
    bstrpick.d $t3, $t1, LA_DIRECT_BITS - 1, 0
    li.w $t4, LA_BOOT_RAM_ROOT_OFFSET
    stx.d $t3, $t0, $t4
    li.w $t3, LA_BOOT_RAM_LEAF
    st.d $t3, $t1, 0
    bl la_bootstrap_mmu
    /* Rebase the active stack before calling compiled runtime code. */
    la.abs $sp, __boot_stack_top
    move $tp, $zero
    move $a0, $a2
    la.abs $t0, la_kernel_main
    jirl $ra, $t0, 0
.Lstop:
    idle 0
    b .Lstop

    .globl la_bootstrap_mmu
la_bootstrap_mmu:
    la.abs $t0, la_tlb_refill
    bstrpick.d $t0, $t0, LA_DIRECT_BITS - 1, 0
    csrwr $t0, 0x88
    li.w $t0, LA_PWCL
    csrwr $t0, 0x1c
    li.w $t0, LA_PWCH
    csrwr $t0, 0x1d
    li.w $t0, 14
    csrwr $t0, 0x1e
    csrwr $zero, 0x19
    la.abs $t0, la_paged_kernel_root
    bstrpick.d $t0, $t0, LA_DIRECT_BITS - 1, 0
    dbar 0
    csrwr $t0, 0x1a
    invtlb 0, $zero, $zero
    jr $ra

    .section .bss.la_boot_tables, "aw", @nobits
    .balign 16384
    .globl la_paged_kernel_root
la_paged_kernel_root:
    .space 16384
    .balign 16384
la_paged_boot_ram:
    .space 16384
