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
        call(40,(long)"/dev/vda",(long)"/root",(long)"ext4",ROOT_MOUNT_FLAGS,0,0)) failed=1;
    if(!failed) {
        long child=call(220,17,0,0,0,0,0);
        if(child==0) {
            if(call(51,(long)"/root",0,0,0,0,0) || call(49,(long)"/",0,0,0,0,0)) call(93,98,0,0,0,0,0);
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
    }
#ifdef ROOT_NETWORK_SETUP
    if(call(39,(long)"/root/proc",0,0,0,0,0)) failed=1;
#endif
    /* reboot 不替 init checkpoint 根盘；先卸载才能直接核对宿主 home blocks。 */
    if(call(39,(long)"/root",0,0,0,0,0)) failed=1;
    puts(failed ? "Linux " ROOT_ARCH_LABEL " root application failed\n" : "Linux " ROOT_ARCH_LABEL " root application passed\n");
    call(142,0xfee1dead,672274793,0x4321fedc,0,0,0);for(;;){}
}
