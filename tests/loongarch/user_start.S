    .text
    .globl _start
_start:
    move $a0, $sp
    bl user_main
    li.w $a7, 93
    syscall 0
.Lstop:
    b .Lstop
