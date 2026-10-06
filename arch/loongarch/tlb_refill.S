    .text
    .align 6
    .globl la_tlb_refill
la_tlb_refill:
    csrwr $t0, 0x8b
    csrrd $t0, 0x1b
    beqz $t0, .Lmissing
    lddir $t0, $t0, 3
    beqz $t0, .Lmissing
    lddir $t0, $t0, 1
    beqz $t0, .Lmissing
    ldpte $t0, 0
    ldpte $t0, 1
    b .Lfill
.Lmissing:
    /* 空目录不能继续把物理零页当页表；先填无效项交给普通缺页入口。 */
    csrwr $zero, 0x8c
    csrwr $zero, 0x8d
    csrrd $t0, 0x8e
    bstrins.d $t0, $zero, 5, 0
    ori $t0, $t0, 14
    csrwr $t0, 0x8e
.Lfill:
    tlbfill
    csrrd $t0, 0x8b
    ertn
