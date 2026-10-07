/* 原程序在child中运行；native wait与exec失败由独立整数父任务报告。 */
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
static int execute(const char *label,const char *cwd,const char *path,
                   const char *const *argv,int adapted,int expected)
{
    message("RUNTIME BEGIN ");message(label);message("\n");
    long child=call(220,17,0,0,0,0,0);
    if(child==0){
        const char *env[]={cwd[1]=='g' ? "LD_LIBRARY_PATH=/glibc/lib" : "LD_LIBRARY_PATH=/musl/lib",
            "PATH=/bin:/musl:.","HOME=/","TERM=vt100",
            adapted ? "LD_PRELOAD=/compat/sched.so" : "LD_PRELOAD=",0};
        if(call(49,(long)cwd,0,0,0,0,0))call(93,98,0,0,0,0,0);
        long result=call(221,(long)path,(long)argv,(long)env,0,0,0);
        message("RUNTIME EXEC ");message(label);message(" errno=");number(-result);message("\n");
        call(93,99,0,0,0,0,0);for(;;){}
    }
    int status=-1;long waited=call(260,child,(long)&status,0,0,0,0);
    message("RUNTIME WAIT ");message(label);message(" status=");number(status);message("\n");
    return child<=0 || waited!=child || status!=expected;
}
int user_main(uint64_t *stack)
{
    (void)stack;int failed=0;
    const char *setup[]={"busybox","sh","/compat/setup.sh",0};
    failed|=execute("setup","/musl","/musl/busybox",setup,0,0);
    if(failed)return 90;
    const char *glibc[]={"busybox","sh","./basic_testcode.sh",0};
    const char *musl[]={"busybox","sh","./basic_testcode.sh",0};
    failed|=execute("basic-original-glibc","/glibc","/glibc/busybox",glibc,0,0);
    failed|=execute("basic-original-musl","/musl","/musl/busybox",musl,0,0);
    /* child已wait后才更换临时盘中的执行文件，原输入文件在宿主保持只读。 */
    if(call(276,-100,(long)"/glibc/basic/brk",-100,(long)"/compat/original-glibc-brk",0,0) ||
       call(276,-100,(long)"/musl/basic/brk",-100,(long)"/compat/original-musl-brk",0,0) ||
       call(276,-100,(long)"/compat/brk-glibc",-100,(long)"/glibc/basic/brk",0,0) ||
       call(276,-100,(long)"/compat/brk-musl",-100,(long)"/musl/basic/brk",0,0)) return 89;
    failed|=execute("basic-adapted-glibc","/glibc","/glibc/busybox",glibc,0,0);
    failed|=execute("basic-adapted-musl","/musl","/musl/busybox",musl,0,0);
    const char *cyclic[]={"cyclictest","-a","-i","1000","-t1","-p99","-D","1s","-q",0};
#ifdef __loongarch__
    const char *probe[]={"sched-probe",0};
    failed|=execute("sched-original","/musl","/compat/sched-probe",probe,0,91<<8);
    failed|=execute("sched-adapted","/musl","/compat/sched-probe",probe,1,0);
    failed|=execute("cyclic-original","/musl","/musl/cyclictest",cyclic,0,1<<8);
    failed|=execute("cyclic-adapted","/musl","/musl/cyclictest",cyclic,1,0);
    const char *group[]={"busybox","sh","./cyclictest_testcode.sh",0};
    failed|=execute("cyclic-group-adapted","/musl","/musl/busybox",group,1,0);
#else
    failed|=execute("cyclic-original","/musl","/musl/cyclictest",cyclic,0,0);
#endif
    /* setup创建的两个子挂载由父fixture持有；退出前先归还，Linux才能卸载根盘。 */
    if(call(39,(long)"/dev/shm",0,0,0,0,0) || call(39,(long)"/proc",0,0,0,0,0))failed=1;
    message("RUNTIME DONE\n");
    return failed ? 88 : 0;
}
