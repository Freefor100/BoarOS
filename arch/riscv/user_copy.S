/* Local-MM copies fault only at the annotated user accesses. Recovery returns
 * the completed prefix to C; page resolution runs after the trap frame is gone.
 * Kernel operands are not annotated: a bad kernel pointer is still fatal. */
#define SUM 0x40000
    .macro recover instruction, target
    .pushsection .rodata.riscv_uaccess_fixups,"a",@progbits
    .dword \instruction, \target
    .popsection
    .endm
    .pushsection .rodata.riscv_uaccess_fixups,"a",@progbits
    .balign 8
    .globl riscv_uaccess_fixups_start
riscv_uaccess_fixups_start:
    .popsection

    .macro copy name, to_user
    .section .text.\name,"ax",@progbits
    .balign 4
    .globl \name
    .type \name,@function
\name:
    li t6,SUM
    csrs sstatus,t6
    beqz a2,.Ldone\@
    xor t1,a0,a1
    andi t1,t1,7
    bnez t1,.Lbytes\@
.Lalign\@:
    andi t1,a0,7
    beqz t1,.Lbatch\@
.Lalign_byte\@:
.Lread_align\@:
    lbu t0,0(a1)
.Lwrite_align\@:
    sb t0,0(a0)
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    beqz a2,.Ldone\@
    j .Lalign\@
.Lbatch\@:
    li t1,32
    bltu a2,t1,.Lwords\@
    .if \to_user
    andi t1,a0,31
    .else
    andi t1,a1,31
    .endif
    bnez t1,.Lword\@
.Lread0\@:
    ld t0,0(a1)
.Lwrite0\@:
    sd t0,0(a0)
.Lread8\@:
    ld t0,8(a1)
.Lwrite8\@:
    sd t0,8(a0)
.Lread16\@:
    ld t0,16(a1)
.Lwrite16\@:
    sd t0,16(a0)
.Lread24\@:
    ld t0,24(a1)
.Lwrite24\@:
    sd t0,24(a0)
    addi a0,a0,32
    addi a1,a1,32
    addi a2,a2,-32
    j .Lbatch\@
.Lwords\@:
    li t1,8
    bltu a2,t1,.Lbytes\@
.Lword\@:
.Lread_word\@:
    ld t0,0(a1)
.Lwrite_word\@:
    sd t0,0(a0)
    addi a0,a0,8
    addi a1,a1,8
    addi a2,a2,-8
    j .Lbatch\@
.Lbytes\@:
    beqz a2,.Ldone\@
.Lread_byte\@:
    lbu t0,0(a1)
.Lwrite_byte\@:
    sb t0,0(a0)
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    j .Lbytes\@
.Ldone\@:
    li a0,0
    li a1,0
    li t6,SUM
    csrc sstatus,t6
    ret
.Lfail24\@:
    addi a2,a2,-24
    j .Lfail\@
.Lfail16\@:
    addi a2,a2,-16
    j .Lfail\@
.Lfail8\@:
    addi a2,a2,-8
.Lfail\@:
    mv a0,a2
    mv a1,t5
    li t6,SUM
    csrc sstatus,t6
    ret
    .if \to_user
    recover .Lwrite_align\@,.Lfail\@
    recover .Lwrite0\@,.Lfail\@
    recover .Lwrite8\@,.Lfail8\@
    recover .Lwrite16\@,.Lfail16\@
    recover .Lwrite24\@,.Lfail24\@
    recover .Lwrite_word\@,.Lfail\@
    recover .Lwrite_byte\@,.Lfail\@
    .else
    recover .Lread_align\@,.Lfail\@
    recover .Lread0\@,.Lfail\@
    recover .Lread8\@,.Lfail8\@
    recover .Lread16\@,.Lfail16\@
    recover .Lread24\@,.Lfail24\@
    recover .Lread_word\@,.Lfail\@
    recover .Lread_byte\@,.Lfail\@
    .endif
    .size \name,.-\name
    .endm
    copy riscv_copy_to_user,1
    copy riscv_copy_from_user,0

    .section .text.riscv_copy_string_from_user,"ax",@progbits
    .balign 4
    .globl riscv_copy_string_from_user
    .type riscv_copy_string_from_user,@function
riscv_copy_string_from_user:
    li t6,SUM
    csrs sstatus,t6
    beqz a2,.Lstring_done
.Lstring_read:
    lbu t0,0(a1)
    sb t0,0(a0)
    beqz t0,.Lstring_done
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    bnez a2,.Lstring_read
.Lstring_done:
    mv a0,a2
    li a1,0
    li t6,SUM
    csrc sstatus,t6
    ret
.Lstring_fault:
    mv a0,a2
    mv a1,t5
    li t6,SUM
    csrc sstatus,t6
    ret
    recover .Lstring_read,.Lstring_fault
    .size riscv_copy_string_from_user,.-riscv_copy_string_from_user

    .pushsection .rodata.riscv_uaccess_fixups,"a",@progbits
    .globl riscv_uaccess_fixups_end
riscv_uaccess_fixups_end:
    .popsection
