#include <arch/loongarch/task.h>
    .text
    .align 6
    .globl la_trap_entry
la_trap_entry:
    /* KS0 在用户态持有任务指针；内核态为零，交换后才能选择可信内核栈。 */
    csrwr $tp, 0x30
    bnez $tp, .Luser
    csrwr $tp, 0x30
    addi.d $sp, $sp, -ARCH_TRAP_FRAME_SIZE
    st.d $t0, $sp, 96
    st.d $tp, $sp, 16
    addi.d $t0, $sp, ARCH_TRAP_FRAME_SIZE
    st.d $t0, $sp, 24
    st.d $zero, $sp, 288
    b .Lsave
.Luser:
    st.d $sp, $tp, 8
    ld.d $sp, $tp, 0
    addi.d $sp, $sp, -ARCH_TRAP_FRAME_SIZE
    st.d $t0, $sp, 96
    ld.d $t0, $tp, 8
    st.d $t0, $sp, 24
    csrrd $t0, 0x30
    st.d $t0, $sp, 16
    st.d $tp, $sp, 288
    csrwr $zero, 0x30
.Lsave:
    st.d $zero, $sp, 0
    st.d $ra, $sp, 8
    st.d $r4, $sp, 32
    st.d $r5, $sp, 40
    st.d $r6, $sp, 48
    st.d $r7, $sp, 56
    st.d $r8, $sp, 64
    st.d $r9, $sp, 72
    st.d $r10, $sp, 80
    st.d $r11, $sp, 88
    st.d $r13, $sp, 104
    st.d $r14, $sp, 112
    st.d $r15, $sp, 120
    st.d $r16, $sp, 128
    st.d $r17, $sp, 136
    st.d $r18, $sp, 144
    st.d $r19, $sp, 152
    st.d $r20, $sp, 160
    st.d $r21, $sp, 168
    st.d $r22, $sp, 176
    st.d $r23, $sp, 184
    st.d $r24, $sp, 192
    st.d $r25, $sp, 200
    st.d $r26, $sp, 208
    st.d $r27, $sp, 216
    st.d $r28, $sp, 224
    st.d $r29, $sp, 232
    st.d $r30, $sp, 240
    st.d $r31, $sp, 248
    csrrd $t0, 1
    st.d $t0, $sp, 256
    csrrd $t0, 6
    st.d $t0, $sp, 264
    csrrd $t0, 5
    st.d $t0, $sp, 272
    csrrd $t0, 7
    st.d $t0, $sp, 280
    move $a0, $sp
    bl la_trap_dispatch
    .globl la_trap_return
la_trap_return:
    move $a0, $sp
    bl la_trap_return_prepare
    ld.d $t0, $sp, 288
    csrwr $t0, 0x30
    ld.d $t0, $sp, 256
    csrwr $t0, 1
    ld.d $t0, $sp, 264
    csrwr $t0, 6
    ld.d $ra, $sp, 8
    ld.d $tp, $sp, 16
    ld.d $r4, $sp, 32
    ld.d $r5, $sp, 40
    ld.d $r6, $sp, 48
    ld.d $r7, $sp, 56
    ld.d $r8, $sp, 64
    ld.d $r9, $sp, 72
    ld.d $r10, $sp, 80
    ld.d $r11, $sp, 88
    ld.d $r12, $sp, 96
    ld.d $r13, $sp, 104
    ld.d $r14, $sp, 112
    ld.d $r15, $sp, 120
    ld.d $r16, $sp, 128
    ld.d $r17, $sp, 136
    ld.d $r18, $sp, 144
    ld.d $r19, $sp, 152
    ld.d $r20, $sp, 160
    ld.d $r21, $sp, 168
    ld.d $r22, $sp, 176
    ld.d $r23, $sp, 184
    ld.d $r24, $sp, 192
    ld.d $r25, $sp, 200
    ld.d $r26, $sp, 208
    ld.d $r27, $sp, 216
    ld.d $r28, $sp, 224
    ld.d $r29, $sp, 232
    ld.d $r30, $sp, 240
    ld.d $r31, $sp, 248
    ld.d $sp, $sp, 24
    ertn
