#include <stdint.h>
#include "../common/raw_syscall.h"
#ifndef EXPECTED_EXIT_STATUS
#define EXPECTED_EXIT_STATUS 0
#endif
#ifndef ROOT_MOUNT_FLAGS
#define ROOT_MOUNT_FLAGS 0
#endif
#ifdef __loongarch__
#define ROOT_ARCH_LABEL "LA"
#else
#define ROOT_ARCH_LABEL "RV"
#endif
#define call test_syscall6
static void puts(const char *s) {unsigned n=0;while(s[n])n++;call(64,1,(long)s,n,0,0,0);}
#ifdef ROOT_REAP_CHILDREN
static int reap_remaining_children(void)
{
    /* 诊断PID1持有退出应用留下的后代；脚本退出不能代替它们的真实wait。 */
    for(;;) {
        int status=-1;
        long child=call(260,-1,(long)&status,0,0,0,0);
        if(child==-10)break;
        if(child<0)return 1;
    }
    puts("Linux " ROOT_ARCH_LABEL " remaining children reaped\n");
    return 0;
}
#endif
int user_main(uint64_t *stack)
{
    (void)stack;
#ifdef ROOT_DIRECT_FILESYSTEM
    /* RV固定profile无initrd；根盘已由Linux挂载，保留真实wait与durable门禁。 */
    long direct_child=call(220,17,0,0,0,0,0);
    if(!direct_child) {
#ifdef ROOT_CREATE_SESSION
        if(call(157,0,0,0,0,0,0)<0)call(93,96,0,0,0,0,0);
#endif
        const char *argv[]={"/init",0},*env[]={0};
        call(221,(long)"/init",(long)argv,(long)env,0,0,0);
        call(93,99,0,0,0,0,0);for(;;){}
    }
    int direct_status=-1;
    int direct_failed=direct_child<0 || call(260,direct_child,(long)&direct_status,0,0,0,0)!=direct_child || direct_status!=(EXPECTED_EXIT_STATUS<<8);
#ifdef ROOT_REAP_CHILDREN
    direct_failed|=reap_remaining_children();
#endif
    long root_fd=call(56,-100,(long)"/",0,0,0,0);
    if(root_fd<0 || call(267,root_fd,0,0,0,0,0) || call(57,root_fd,0,0,0,0,0))direct_failed=1;
    puts(direct_failed ? "Linux " ROOT_ARCH_LABEL " root application failed\n" : "Linux " ROOT_ARCH_LABEL " root application passed\n");
    call(142,0xfee1dead,672274793,0x4321fedc,0,0,0);for(;;){}
#endif
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
        call(40,(long)"/dev/vda",(long)"/root",(long)"ext4",ROOT_MOUNT_FLAGS,0,0)) failed=1;
    if(!failed) {
        long child=call(220,17,0,0,0,0,0);
        if(child==0) {
            if(call(51,(long)"/root",0,0,0,0,0) || call(49,(long)"/",0,0,0,0,0)) call(93,98,0,0,0,0,0);
#ifdef ROOT_CREATE_SESSION
            /* 原Linux init继承PGID0；监督入口需要真实存活的正进程组身份。 */
            if(call(157,0,0,0,0,0,0)<0)call(93,96,0,0,0,0,0);
#endif
#ifdef ROOT_NETWORK_SETUP
            if(call(34,-100,(long)"/proc",0755,0,0,0)<0 ||
               call(40,(long)"proc",(long)"/proc",(long)"proc",0,0,0)) call(93,97,0,0,0,0,0);
            long socket=call(198,2,2,0,0,0,0);
            struct {char name[16];unsigned short flags;unsigned char rest[22];} interface={.name="lo"};
            if(socket<0 || call(29,socket,0x8913,(long)&interface,0,0,0)) call(93,96,0,0,0,0,0);
            interface.flags|=1;
            if(call(29,socket,0x8914,(long)&interface,0,0,0)) call(93,96,0,0,0,0,0);
            call(57,socket,0,0,0,0,0);
#endif
            const char *argv[]={"/init",0},*env[]={0};
            call(221,(long)"/init",(long)argv,(long)env,0,0,0);call(93,99,0,0,0,0,0);
            for(;;){}
        }
        int status=-1;
        if(call(260,child,(long)&status,0,0,0,0)!=child || status!=(EXPECTED_EXIT_STATUS<<8)) failed=1;
#ifdef ROOT_REAP_CHILDREN
        failed|=reap_remaining_children();
#endif
    }
#if defined(ROOT_NETWORK_SETUP) || defined(ROOT_PROC_CLEANUP)
    for(;;) {
        long status=call(39,(long)"/root/proc",0,0,0,0,0);
        if(!status)continue;
        if(status!=-22)failed=1;
        break;
    }
#endif
    /* reboot 不替 init checkpoint 根盘；先卸载才能直接核对宿主 home blocks。 */
    if(call(39,(long)"/root",0,0,0,0,0)) failed=1;
    puts(failed ? "Linux " ROOT_ARCH_LABEL " root application failed\n" : "Linux " ROOT_ARCH_LABEL " root application passed\n");
    call(142,0xfee1dead,672274793,0x4321fedc,0,0,0);for(;;){}
}
