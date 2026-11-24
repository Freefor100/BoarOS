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
    .if \to_user
    bnez t1,.Ldst_align\@
    .else
    bnez t1,.Lbytes\@
    .endif
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
    /* The aligned loop advances by a whole batch, preserving alignment
     * and the fault offsets. Only the remaining length can end it. */
    li t1,32
.Lread0\@:
    ld t0,0(a1)
    .if \to_user
    /* Kernel source words are already in range. Read all four before
     * touching the user mapping so alternating translations cannot evict
     * each other on every word. Each user store has its own fixup. */
    ld t2,8(a1)
    ld t3,16(a1)
    ld t4,24(a1)
.Lwrite0\@:
    sd t0,0(a0)
.Lwrite8\@:
    sd t2,8(a0)
.Lwrite16\@:
    sd t3,16(a0)
.Lwrite24\@:
    sd t4,24(a0)
    .else
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
    .endif
    addi a0,a0,32
    addi a1,a1,32
    addi a2,a2,-32
    bgeu a2,t1,.Lread0\@
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
    .if \to_user
/* Align the user stores, then assemble each word from in-range kernel
 * loads. Each recovered store still has exactly a2 bytes remaining.
 * User reads keep their byte path so a missing next page cannot discard
 * a valid prefix while assembling a word. */
.Ldst_align\@:
    andi t1,a0,7
    beqz t1,.Lshift_prepare\@
    lbu t0,0(a1)
.Lwrite_dst_align\@:
    sb t0,0(a0)
    addi a0,a0,1
    addi a1,a1,1
    addi a2,a2,-1
    bnez a2,.Ldst_align\@
    j .Ldone\@
.Lshift_prepare\@:
    li t1,16
    bltu a2,t1,.Lbytes\@
    andi t3,a1,7
    neg t4,a1
    andi t4,t4,7
    add a3,a1,t4
    addi a4,t4,8
    slli t3,t3,3
    slli t4,t4,3
    mv a5,a1
    li t2,0
    li a6,0
.Lshift_prefix\@:
    lbu t0,0(a5)
    sll t0,t0,a6
    or t2,t2,t0
    addi a6,a6,8
    addi a5,a5,1
    bne a5,a3,.Lshift_prefix\@
.Lshift_words\@:
    ld a5,0(a3)
    sll t0,a5,t4
    or t0,t0,t2
.Lwrite_shift_word\@:
    sd t0,0(a0)
    srl t2,a5,t3
    addi a0,a0,8
    addi a1,a1,8
    addi a2,a2,-8
    addi a3,a3,8
    bgeu a2,a4,.Lshift_words\@
    .endif
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
    recover .Lwrite_dst_align\@,.Lfail\@
    recover .Lwrite_shift_word\@,.Lfail\@
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

#if BOAROS_MAX_CPUS == 1
    .section .text.riscv_copy_page,"ax",@progbits
    .balign 4
    .globl riscv_copy_page
    .type riscv_copy_page,@function
riscv_copy_page:
    lui a2,1
    add a2,a2,a0
.Lcopy_page:
    /* Read the entire cache line before storing it, even when the source
     * and destination evict each other's translation. */
    ld t0,0(a1)
    ld t1,8(a1)
    ld t2,16(a1)
    ld t3,24(a1)
    ld t4,32(a1)
    ld a3,40(a1)
    ld a4,48(a1)
    ld a5,56(a1)
    sd t0,0(a0)
    sd t1,8(a0)
    sd t2,16(a0)
    sd t3,24(a0)
    sd t4,32(a0)
    sd a3,40(a0)
    sd a4,48(a0)
    sd a5,56(a0)
    addi a0,a0,64
    addi a1,a1,64
    bne a0,a2,.Lcopy_page
    ret
    .size riscv_copy_page,.-riscv_copy_page
#endif

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
