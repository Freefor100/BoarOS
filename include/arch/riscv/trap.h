#ifndef BOAROS_ARCH_RISCV_TRAP_H
#define BOAROS_ARCH_RISCV_TRAP_H

#define RISCV_TRAP_FRAME_RA 0
#define RISCV_TRAP_FRAME_SP 8
#define RISCV_TRAP_FRAME_GP 16
#define RISCV_TRAP_FRAME_TP 24
#define RISCV_TRAP_FRAME_T0 32
#define RISCV_TRAP_FRAME_T1 40
#define RISCV_TRAP_FRAME_T2 48
#define RISCV_TRAP_FRAME_S0 56
#define RISCV_TRAP_FRAME_S1 64
#define RISCV_TRAP_FRAME_A0 72
#define RISCV_TRAP_FRAME_A1 80
#define RISCV_TRAP_FRAME_A2 88
#define RISCV_TRAP_FRAME_A3 96
#define RISCV_TRAP_FRAME_A4 104
#define RISCV_TRAP_FRAME_A5 112
#define RISCV_TRAP_FRAME_A6 120
#define RISCV_TRAP_FRAME_A7 128
#define RISCV_TRAP_FRAME_S2 136
#define RISCV_TRAP_FRAME_S3 144
#define RISCV_TRAP_FRAME_S4 152
#define RISCV_TRAP_FRAME_S5 160
#define RISCV_TRAP_FRAME_S6 168
#define RISCV_TRAP_FRAME_S7 176
#define RISCV_TRAP_FRAME_S8 184
#define RISCV_TRAP_FRAME_S9 192
#define RISCV_TRAP_FRAME_S10 200
#define RISCV_TRAP_FRAME_S11 208
#define RISCV_TRAP_FRAME_T3 216
#define RISCV_TRAP_FRAME_T4 224
#define RISCV_TRAP_FRAME_T5 232
#define RISCV_TRAP_FRAME_T6 240
#define RISCV_TRAP_FRAME_SSTATUS 248
#define RISCV_TRAP_FRAME_SEPC 256
#define RISCV_TRAP_FRAME_SCAUSE 264
#define RISCV_TRAP_FRAME_STVAL 272
#define RISCV_TRAP_FRAME_SIZE 288

#define RISCV_SSTATUS_SIE 0x2
#define RISCV_SSTATUS_SPIE 0x20
#define RISCV_SSTATUS_SPP 0x100

#define RISCV_SCAUSE_INTERRUPT 0x8000000000000000
#define RISCV_SCAUSE_SUPERVISOR_SOFTWARE 1
#define RISCV_SCAUSE_BREAKPOINT 3
#define RISCV_SCAUSE_SUPERVISOR_TIMER 5

#ifndef __ASSEMBLER__

#include <stddef.h>

struct riscv_trap_frame {
    unsigned long ra;
    unsigned long sp;
    unsigned long gp;
    unsigned long tp;
    unsigned long t0;
    unsigned long t1;
    unsigned long t2;
    unsigned long s0;
    unsigned long s1;
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    unsigned long a3;
    unsigned long a4;
    unsigned long a5;
    unsigned long a6;
    unsigned long a7;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
    unsigned long t3;
    unsigned long t4;
    unsigned long t5;
    unsigned long t6;
    unsigned long sstatus;
    unsigned long sepc;
    unsigned long scause;
    unsigned long stval;
} __attribute__((aligned(16)));

#define RISCV_TRAP_ASSERT_OFFSET(member, offset) \
    _Static_assert(offsetof(struct riscv_trap_frame, member) == (offset), \
                   "riscv trap frame offset mismatch: " #member)

_Static_assert(sizeof(unsigned long) == 8U,
               "RISC-V trap frame requires RV64");
RISCV_TRAP_ASSERT_OFFSET(ra, RISCV_TRAP_FRAME_RA);
RISCV_TRAP_ASSERT_OFFSET(sp, RISCV_TRAP_FRAME_SP);
RISCV_TRAP_ASSERT_OFFSET(gp, RISCV_TRAP_FRAME_GP);
RISCV_TRAP_ASSERT_OFFSET(tp, RISCV_TRAP_FRAME_TP);
RISCV_TRAP_ASSERT_OFFSET(t0, RISCV_TRAP_FRAME_T0);
RISCV_TRAP_ASSERT_OFFSET(t1, RISCV_TRAP_FRAME_T1);
RISCV_TRAP_ASSERT_OFFSET(t2, RISCV_TRAP_FRAME_T2);
RISCV_TRAP_ASSERT_OFFSET(s0, RISCV_TRAP_FRAME_S0);
RISCV_TRAP_ASSERT_OFFSET(s1, RISCV_TRAP_FRAME_S1);
RISCV_TRAP_ASSERT_OFFSET(a0, RISCV_TRAP_FRAME_A0);
RISCV_TRAP_ASSERT_OFFSET(a1, RISCV_TRAP_FRAME_A1);
RISCV_TRAP_ASSERT_OFFSET(a2, RISCV_TRAP_FRAME_A2);
RISCV_TRAP_ASSERT_OFFSET(a3, RISCV_TRAP_FRAME_A3);
RISCV_TRAP_ASSERT_OFFSET(a4, RISCV_TRAP_FRAME_A4);
RISCV_TRAP_ASSERT_OFFSET(a5, RISCV_TRAP_FRAME_A5);
RISCV_TRAP_ASSERT_OFFSET(a6, RISCV_TRAP_FRAME_A6);
RISCV_TRAP_ASSERT_OFFSET(a7, RISCV_TRAP_FRAME_A7);
RISCV_TRAP_ASSERT_OFFSET(s2, RISCV_TRAP_FRAME_S2);
RISCV_TRAP_ASSERT_OFFSET(s3, RISCV_TRAP_FRAME_S3);
RISCV_TRAP_ASSERT_OFFSET(s4, RISCV_TRAP_FRAME_S4);
RISCV_TRAP_ASSERT_OFFSET(s5, RISCV_TRAP_FRAME_S5);
RISCV_TRAP_ASSERT_OFFSET(s6, RISCV_TRAP_FRAME_S6);
RISCV_TRAP_ASSERT_OFFSET(s7, RISCV_TRAP_FRAME_S7);
RISCV_TRAP_ASSERT_OFFSET(s8, RISCV_TRAP_FRAME_S8);
RISCV_TRAP_ASSERT_OFFSET(s9, RISCV_TRAP_FRAME_S9);
RISCV_TRAP_ASSERT_OFFSET(s10, RISCV_TRAP_FRAME_S10);
RISCV_TRAP_ASSERT_OFFSET(s11, RISCV_TRAP_FRAME_S11);
RISCV_TRAP_ASSERT_OFFSET(t3, RISCV_TRAP_FRAME_T3);
RISCV_TRAP_ASSERT_OFFSET(t4, RISCV_TRAP_FRAME_T4);
RISCV_TRAP_ASSERT_OFFSET(t5, RISCV_TRAP_FRAME_T5);
RISCV_TRAP_ASSERT_OFFSET(t6, RISCV_TRAP_FRAME_T6);
RISCV_TRAP_ASSERT_OFFSET(sstatus, RISCV_TRAP_FRAME_SSTATUS);
RISCV_TRAP_ASSERT_OFFSET(sepc, RISCV_TRAP_FRAME_SEPC);
RISCV_TRAP_ASSERT_OFFSET(scause, RISCV_TRAP_FRAME_SCAUSE);
RISCV_TRAP_ASSERT_OFFSET(stval, RISCV_TRAP_FRAME_STVAL);
_Static_assert(sizeof(struct riscv_trap_frame) == RISCV_TRAP_FRAME_SIZE,
               "riscv trap frame size mismatch");

#undef RISCV_TRAP_ASSERT_OFFSET

void riscv_trap_dispatch(struct riscv_trap_frame *frame);
void riscv_trap_bad_return(struct riscv_trap_frame *frame)
    __attribute__((noreturn));

#endif

#endif
