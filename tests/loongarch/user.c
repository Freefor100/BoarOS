#include <stdint.h>
#include <stddef.h>
static long call6(long number,long a,long b,long c,long d,long e,long f)
{
    register long r4 __asm__("a0")=a, r5 __asm__("a1")=b, r6 __asm__("a2")=c;
    register long r7 __asm__("a3")=d, r8 __asm__("a4")=e, r9 __asm__("a5")=f;
    register long r11 __asm__("a7")=number;
    __asm__ volatile("syscall 0" : "+r"(r4) : "r"(r5),"r"(r6),"r"(r7),"r"(r8),"r"(r9),"r"(r11) : "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8", "memory");
    return r4;
}
#define CALL(n,a,b,c) call6(n,(long)(a),(long)(b),(long)(c),0,0,0)
struct timespec { long sec, ns; };
static volatile unsigned char zero_tail[16417];
static const volatile unsigned char full_file[16457] __attribute__((aligned(16384))) = { [0]=0x5a, [16383]=0xa5, [16384]=0x33, [16456]=0x44 };
static volatile uint64_t initialized=0x123456789abcdef0;
static int equal(const char *a,const char *b)
{ while (*a && *a==*b) { a++; b++; } return *a==*b; }
static uint64_t counter(void) { uint64_t v; __asm__ volatile("rdtime.d %0, $zero":"=r"(v)); return v; }
static uint64_t frequency(void)
{
    uint64_t base, ratio, index=4;
    __asm__ volatile("cpucfg %0, %1":"=r"(base):"r"(index)); index=5;
    __asm__ volatile("cpucfg %0, %1":"=r"(ratio):"r"(index));
    return base*(ratio&0xffff)/((ratio>>16)&0xffff);
}
static int spin(void)
{
    volatile unsigned *shared=(void *)call6(222,0,16384,3,0x21,-1,0);
    if ((long)shared<0) return 70;
    long child=call6(220,17,0,0,0,0,0);
    if (child<0) return 71;
    unsigned index=child==0;
    __atomic_store_n(&shared[index],1,__ATOMIC_RELEASE);
    uint64_t start=counter(), deadline=start+frequency()*3;
    uint64_t accumulator=0;
    /* Neither peer yields or enters a syscall while waiting for the other CPU turn. */
    while (!__atomic_load_n(&shared[1-index],__ATOMIC_ACQUIRE)) {
        accumulator++;
        if ((int64_t)(counter()-deadline)>=0) return 72;
    }
    uint64_t stop=counter()+frequency()/20;
    while ((int64_t)(counter()-stop)<0) accumulator=(accumulator*33)^0x9876;
    __atomic_store_n(&shared[index+2],1,__ATOMIC_RELEASE);
    if (!child) { CALL(93,0,0,0); for (;;) { } }
    int status=-1;
    if (call6(260,child,(long)&status,0,0,0,0)!=child || status || !shared[2] || !shared[3]) return 73;
    if (CALL(215,shared,16384,0)) return 74;
    __asm__ volatile(""::"r"(accumulator):"$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7","$t8","memory");
    return 0;
}
int user_register_probe(void);
static int console(void)
{
    long fd=0;
    const char poll_ready[]="LA console poll ready\n";
    if (CALL(64,fd,poll_ready,sizeof(poll_ready)-1)!=(long)sizeof(poll_ready)-1) return 82;
    struct { int fd; short events,revents; } poll={(int)fd,1,0};
    if (call6(73,(long)&poll,1,0,0,8,0)!=1 || !(poll.revents&1)) return 83;
    char byte;
    if (CALL(63,fd,&byte,1)!=1 || byte!='g' || CALL(63,fd,&byte,1)!=1 || byte!='\n') return 84;
    const char read_ready[]="LA console read ready\n";
    if (CALL(64,fd,read_ready,sizeof(read_ready)-1)!=(long)sizeof(read_ready)-1) return 85;
    if (CALL(63,fd,&byte,1)!=1 || byte!='r' || CALL(63,fd,&byte,1)!=1 || byte!='\n') return 86;
    if (CALL(57,fd,0,0)) return 87;
    return 0;
}
int user_main(uint64_t *stack)
{
    if (((uintptr_t)stack&15) || !stack[0]) return 1;
    char **argv=(void *)(stack+1); uint64_t *aux=(void *)(argv+stack[0]+1);
    while (*aux) aux++; aux++;
    unsigned page=0;
    while (aux[0]) { if (aux[0]==6) page=(unsigned)aux[1]; aux+=2; }
    if (page!=16384 || initialized!=0x123456789abcdef0 || full_file[0]!=0x5a || full_file[16383]!=0xa5 || full_file[16384]!=0x33 || full_file[16456]!=0x44) return 2;
    for (unsigned i=0;i<sizeof(zero_tail);i++) if (zero_tail[i]) return 3;
    if (user_register_probe()) return 15;
    const char *mode=stack[0]>1 ? argv[1] : "contracts";
    if (equal(mode,"spin")) return spin();
    if (equal(mode,"exitgroup")) { CALL(94,0,0,0);return 90; }
    if (equal(mode,"console")) return console();
    volatile unsigned char *memory=(void *)call6(222,0,page*3,3,0x22,-1,0);
    if ((long)memory<0) return 4;
    memory[page-1]=42; memory[page]=43; memory[page*2]=44;
    if (equal(mode,"readonly")) { CALL(226,memory,page,1); memory[0]=1; return 90; }
    if (equal(mode,"none")) { CALL(226,memory,page,0); return memory[0]+90; }
    if (equal(mode,"unmapped")) { CALL(215,memory,page*3,0); memory[0]=1; return 90; }
    if (equal(mode,"nonexec")) {
        *(volatile uint32_t *)memory=0x4c000020; __asm__ volatile("ibar 0":::"memory");
        ((void (*)(void))(uintptr_t)memory)(); return 90;
    }
    if (equal(mode,"kernel")) return *(volatile unsigned char *)(uintptr_t)0x9000000000200000;
    if (CALL(172,0,0,0)<=0 || CALL(178,0,0,0)!=CALL(172,0,0,0)) return 5;
    if (CALL(0xffffffff,0,0,0)!=-38) return 6;
    struct timespec first,second,delay={0,20000000};
    if (CALL(113,1,&first,0) || CALL(113,1,0,0)!=-14) return 7;
    if (CALL(101,&delay,0,0) || CALL(113,1,&second,0)) return 8;
    if ((second.sec-first.sec)*1000000000+second.ns-first.ns<20000000) return 9;
    if (CALL(124,0,0,0)) return 10;
    if (CALL(113,1,memory+page-8,0) || ((volatile struct timespec *)(memory+page-8))->ns>=1000000000) return 16;
    memory[page-1]=42;memory[page]=43;
    if (CALL(226,memory,page,1) || memory[page-1]!=42 || memory[page]!=43) return 11;
    if (CALL(226,memory,page,3)) return 12;
    memory[page-1]=45;
    if (CALL(226,memory+1,page,1)!=-22) return 13;
    if (CALL(215,memory,page*3,0)) return 14;
    return 0;
}
