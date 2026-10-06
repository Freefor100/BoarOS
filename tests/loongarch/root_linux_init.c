#include <stdint.h>
static long call(long number,long a,long b,long c,long d,long e,long f)
{
    register long r4 __asm__("a0")=a,r5 __asm__("a1")=b,r6 __asm__("a2")=c;
    register long r7 __asm__("a3")=d,r8 __asm__("a4")=e,r9 __asm__("a5")=f,r11 __asm__("a7")=number;
    __asm__ volatile("syscall 0":"+r"(r4):"r"(r5),"r"(r6),"r"(r7),"r"(r8),"r"(r9),"r"(r11):"$t0","$t1","$t2","$t3","$t4","$t5","$t6","$t7","$t8","memory");return r4;
}
static void puts(const char *s) {unsigned n=0;while(s[n])n++;call(64,1,(long)s,n,0,0,0);}
int user_main(uint64_t *stack)
{
    (void)stack;
    long console=call(56,-100,(long)"/dev/console",2,0,0,0);
    call(24,console,1,0,0,0,0);call(24,console,2,0,0,0,0);
    call(34,-100,(long)"/sys",0755,0,0,0);call(34,-100,(long)"/root",0755,0,0,0);
    int failed=call(40,(long)"sysfs",(long)"/sys",(long)"sysfs",0,0,0)!=0;
    char device[64];long fd=call(56,-100,(long)"/sys/class/block/vda/dev",0,0,0,0);
    long n=call(63,fd,(long)device,sizeof(device)-1,0,0,0);call(57,fd,0,0,0,0,0);
    unsigned major=0,minor=0,i=0;
    if(n<=0) failed=1;
    else {
        while(i<(unsigned)n && device[i]>='0' && device[i]<='9') major=major*10+device[i++]-'0';
        if(i>=(unsigned)n || device[i++]!=':') failed=1;
        while(i<(unsigned)n && device[i]>='0' && device[i]<='9') minor=minor*10+device[i++]-'0';
    }
    unsigned encoded=(major<<8)|(minor&255)|((minor&~255U)<<12);
    if(call(33,-100,(long)"/dev/vda",060600,encoded,0,0) ||
        call(40,(long)"/dev/vda",(long)"/root",(long)"ext4",0,0,0)) failed=1;
    if(!failed) {
        long child=call(220,17,0,0,0,0,0);
        if(child==0) {
            if(call(51,(long)"/root",0,0,0,0,0) || call(49,(long)"/",0,0,0,0,0)) call(93,98,0,0,0,0,0);
            const char *argv[]={"/init",0},*env[]={0};
            call(221,(long)"/init",(long)argv,(long)env,0,0,0);call(93,99,0,0,0,0,0);
            for(;;){}
        }
        int status=-1;
        if(call(260,child,(long)&status,0,0,0,0)!=child || status) failed=1;
    }
    puts(failed ? "Linux LA root application failed\n" : "Linux LA root application passed\n");
    call(142,0xfee1dead,672274793,0x4321fedc,0,0,0);for(;;){}
}
