    .text
    .globl la_signal_register_probe
la_signal_register_probe:
    addi.d $sp, $sp, -112
    st.d $ra, $sp, 0
    st.d $tp, $sp, 8
    st.d $fp, $sp, 16
    st.d $s0, $sp, 24
    st.d $s1, $sp, 32
    st.d $s2, $sp, 40
    st.d $s3, $sp, 48
    st.d $s4, $sp, 56
    st.d $s5, $sp, 64
    st.d $s6, $sp, 72
    st.d $s7, $sp, 80
    st.d $s8, $sp, 88
    st.d $r21, $sp, 96
    move $a3, $sp
    li.d $fp, 819
    li.d $r21, 1365
    li.d $t0, 256
    li.d $t1, 257
    li.d $t2, 258
    li.d $t3, 259
    li.d $t4, 260
    li.d $t5, 261
    li.d $t6, 262
    li.d $t7, 263
    li.d $t8, 264
    li.d $s0, 512
    li.d $s1, 513
    li.d $s2, 514
    li.d $s3, 515
    li.d $s4, 516
    li.d $s5, 517
    li.d $s6, 518
    li.d $s7, 519
    li.d $s8, 520
    break 0
    bne $sp, $a3, .Lbad
    ld.d $a2, $sp, 8
    bne $tp, $a2, .Lbad
    li.d $a2, 819
    bne $fp, $a2, .Lbad
    li.d $a2, 1365
    bne $r21, $a2, .Lbad
    li.d $a2, 256
    bne $t0, $a2, .Lbad
    li.d $a2, 257
    bne $t1, $a2, .Lbad
    li.d $a2, 258
    bne $t2, $a2, .Lbad
    li.d $a2, 259
    bne $t3, $a2, .Lbad
    li.d $a2, 260
    bne $t4, $a2, .Lbad
    li.d $a2, 261
    bne $t5, $a2, .Lbad
    li.d $a2, 262
    bne $t6, $a2, .Lbad
    li.d $a2, 263
    bne $t7, $a2, .Lbad
    li.d $a2, 264
    bne $t8, $a2, .Lbad
    li.d $a2, 512
    bne $s0, $a2, .Lbad
    li.d $a2, 513
    bne $s1, $a2, .Lbad
    li.d $a2, 514
    bne $s2, $a2, .Lbad
    li.d $a2, 515
    bne $s3, $a2, .Lbad
    li.d $a2, 516
    bne $s4, $a2, .Lbad
    li.d $a2, 517
    bne $s5, $a2, .Lbad
    li.d $a2, 518
    bne $s6, $a2, .Lbad
    li.d $a2, 519
    bne $s7, $a2, .Lbad
    li.d $a2, 520
    bne $s8, $a2, .Lbad
    move $a0, $zero
    b .Lrestore
.Lbad:
    li.d $a0, 1
.Lrestore:
    ld.d $ra, $sp, 0
    ld.d $tp, $sp, 8
    ld.d $fp, $sp, 16
    ld.d $s0, $sp, 24
    ld.d $s1, $sp, 32
    ld.d $s2, $sp, 40
    ld.d $s3, $sp, 48
    ld.d $s4, $sp, 56
    ld.d $s5, $sp, 64
    ld.d $s6, $sp, 72
    ld.d $s7, $sp, 80
    ld.d $s8, $sp, 88
    ld.d $r21, $sp, 96
    addi.d $sp, $sp, 112
    jr $ra
