    .section .text, "ax", @progbits
    .align 2
    .globl trap_test_breakpoint
    .type trap_test_breakpoint, @function

trap_test_breakpoint:
    ebreak
    ret

    .size trap_test_breakpoint, . - trap_test_breakpoint
