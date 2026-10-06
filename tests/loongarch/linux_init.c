#include <stdint.h>
static long call6(long number,long a,long b,long c,long d,long e,long f)
{
    register long r4 __asm__("a0")=a,r5 __asm__("a1")=b,r6 __asm__("a2")=c;
    register long r7 __asm__("a3")=d,r8 __asm__("a4")=e,r9 __asm__("a5")=f,r11 __asm__("a7")=number;
    __asm__ volatile("syscall 0":"+r"(r4):"r"(r5),"r"(r6),"r"(r7),"r"(r8),"r"(r9),"r"(r11):"$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7","$t8","memory");
    return r4;
}
static void puts(const char *s)
{ unsigned n=0;while(s[n])n++;call6(64,1,(long)s,n,0,0,0); }
static void hex(uint64_t n)
{
    char s[19]="0x0000000000000000";
    for(unsigned i=0;i<16;i++)s[17-i]="0123456789abcdef"[(n>>(i*4))&15];puts(s);
}
int user_main(uint64_t *stack)
{
    (void)stack;
    long console=call6(56,-100,(long)"/dev/console",2,0,0,0);
    if (console!=1) call6(24,console,1,0,0,0,0);
    if (console!=2) call6(24,console,2,0,0,0,0);
    const char *cases[]={"contracts","spin","exitgroup","readonly","none","nonexec","unmapped","kernel","console"};
    unsigned signals[]={0,0,0,11,11,11,11,7,0};
    int failures=0;
    for(unsigned i=0;i<9;i++) {
        long child=call6(220,17,0,0,0,0,0);
        if (!child) {
            const char *argv[]={"/la-probe",cases[i],0};const char *env[]={0};
            call6(221,(long)argv[0],(long)argv,(long)env,0,0,0);
            call6(93,99,0,0,0,0,0);for(;;){}
        }
        int status=-1;
        long waited=call6(260,child,(long)&status,0,0,0,0);
        unsigned signal=status&0x7f, code=signal ? signal : (unsigned)status>>8;
        puts("LA case ");puts(cases[i]);puts(" reason=");hex(signal ? 4 : 1);puts(" status=");hex(code);puts("\n");
        if (waited!=child || (signals[i] ? signal!=signals[i] : status!=0)) failures++;
    }
    puts(failures ? "Linux LA user contracts failed\n" : "Linux LA user contracts passed\n");
    call6(142,0xfee1dead,672274793,0x4321fedc,0,0,0);
    for(;;){}
}
