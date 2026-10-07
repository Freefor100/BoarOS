/* 原ELF由独立child执行；父任务只报告native状态，不依赖被调查的libc。 */
#include <stdint.h>
#include "../common/raw_syscall.h"
#define call test_syscall6
static void message(const char *s)
{
    unsigned n=0;while(s[n])n++;
    call(64,1,(long)s,n,0,0,0);
}
static void number(long value)
{
    char buffer[24];unsigned n=0;
    if(value<0){message("-");value=-value;}
    do{buffer[n++]=(char)('0'+value%10);value/=10;}while(value);
    while(n){char byte=buffer[--n];call(64,1,(long)&byte,1,0,0,0);}
}
int user_main(uint64_t *stack)
{
    (void)stack;
#ifdef REQUIRE_RANDOM_READY
    /* 真实RNG完成后才启动callee；不以seed或时钟伪造ready。 */
    if(call(278,0,0,0,0,0,0))return 91;
#endif
    long child=call(220,17,0,0,0,0,0);
    if(!child){
        const char *argv[]={"entry-static.exe","clock_gettime",0};
        const char *env[]={"LD_LIBRARY_PATH=/glibc/lib","PATH=/bin:/glibc:.","HOME=/","TERM=vt100",0};
        if(call(49,(long)"/glibc",0,0,0,0,0))call(93,98,0,0,0,0,0);
        long result=call(221,(long)"/glibc/entry-static.exe",(long)argv,(long)env,0,0,0);
        message("AUDIT EXEC errno=");number(-result);message("\n");
        call(93,99,0,0,0,0,0);for(;;){}
    }
    int status=-1;long waited=call(260,child,(long)&status,0,0,0,0);
    message("AUDIT WAIT pid=");number(waited);message(" status=");number(status);message("\n");
    return child>0 && waited==child ? 0 : 90;
}
