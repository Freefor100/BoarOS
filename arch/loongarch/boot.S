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
    la.abs $t0, .Lmapped
    jr $t0
.Lmapped:
    csrwr $zero, 0x182
    la.abs $t0, __bss_start
    la.abs $t1, __bss_end
.Lzero:
    bgeu $t0, $t1, .Lready
    st.d $zero, $t0, 0
    addi.d $t0, $t0, 8
    b .Lzero
.Lready:
    la.abs $sp, __boot_stack_top
    move $tp, $zero
    move $a0, $a2
    bl la_kernel_main
.Lstop:
    idle 0
    b .Lstop
