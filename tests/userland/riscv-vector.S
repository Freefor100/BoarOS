    .text
    .option push
    .option arch,+v
    .globl vector_load
vector_load:
    csrw vstart,zero
    csrr t0,vlenb
    slli t0,t0,3
    vl8re8.v v0,(a0)
    add a0,a0,t0
    vl8re8.v v8,(a0)
    add a0,a0,t0
    vl8re8.v v16,(a0)
    add a0,a0,t0
    vl8re8.v v24,(a0)
    add a0,a0,t0
    ret
    .globl vector_store
vector_store:
    csrw vstart,zero
    csrr t0,vlenb
    slli t0,t0,3
    vs8r.v v0,(a0)
    add a0,a0,t0
    vs8r.v v8,(a0)
    add a0,a0,t0
    vs8r.v v16,(a0)
    add a0,a0,t0
    vs8r.v v24,(a0)
    add a0,a0,t0
    ret
    .option pop
