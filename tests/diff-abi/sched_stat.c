#include "abi.h"

static long read_stat_field(long pid,unsigned wanted)
{
    char path[80], digits[24];
    static const char prefix[]="/schedstat-proc/";
    unsigned length=sizeof(prefix)-1U,n=0;
    for(unsigned i=0;i<length;i++) path[i]=prefix[i];
    do { digits[n++]=(char)('0'+pid%10); pid/=10; } while(pid);
    while(n) path[length++]=digits[--n];
    static const char tail[]="/stat";
    for(unsigned i=0;i<sizeof(tail);i++) path[length++]=tail[i];
    long fd=SC4(56,-100,path,0,0); abi_require(fd>=0);
    char text[1024]; long count=SC3(63,fd,text,sizeof(text)-1);
    abi_require(count>0); text[count]=0; abi_require(SC1(57,fd)==0);
    char *cursor=text;
    for(char *p=text;*p;p++) if(*p==')') cursor=p+4;
    if(wanted==3) return cursor[-2];
    for(unsigned field=4;field<=wanted;field++) {
        while(*cursor==' ') cursor++;
        int negative=*cursor=='-'; if(negative) cursor++;
        abi_require(*cursor>='0' && *cursor<='9');
        unsigned long value=0;
        while(*cursor>='0' && *cursor<='9') value=value*10+(unsigned)(*cursor++-'0');
        if(field==wanted) return negative?-(long)value:(long)value;
    }
    return -1;
}
void abi_sched_stat_cases(void)
{
    abi_require(SC3(34,-100,"/schedstat-proc",0755)==0);
    abi_require(SC5(40,"proc","/schedstat-proc","proc",0,0)==0);
    long self=SC0(172); int param=0;
    abi_record("sched.stat-other",read_stat_field(self,18),-1,-1,0,0,0);
    param=23; abi_require(SC3(119,0,1,&param)==0);
    long fifo[3]={read_stat_field(self,18),read_stat_field(self,40),read_stat_field(self,41)};
    abi_record("sched.stat-fifo",0,-1,-1,0,fifo,sizeof(fifo));
    param=35; abi_require(SC3(119,0,2,&param)==0);
    long rr[3]={read_stat_field(self,18),read_stat_field(self,40),read_stat_field(self,41)};
    abi_record("sched.stat-rr",0,-1,-1,0,rr,sizeof(rr));
    param=0; abi_require(SC3(119,0,0,&param)==0);
    long child=SC5(220,17,0,0,0,0); abi_require(child>=0);
    if(!child) { abi_require(SC2(129,SC0(172),19)==0); abi_exit(0); }
    int status; abi_require(SC4(260,child,&status,2,0)==child);
    abi_record("sched.stat-stopped-wchan",read_stat_field(child,35),-1,-1,0,0,0);
    abi_require(SC2(129,child,18)==0);
    long delay[2]={0,1000000};
    unsigned tries=0;
    while(read_stat_field(child,3)!='Z' && tries++<1000) abi_require(SC2(101,delay,0)==0);
    abi_require(tries<1000);
    abi_record("sched.stat-zombie-wchan",read_stat_field(child,35),-1,-1,0,0,0);
    abi_require(SC4(260,child,&status,0,0)==child && status==0);
    abi_require(SC2(39,"/schedstat-proc",0)==0);
    abi_require(SC3(35,-100,"/schedstat-proc",0x200)==0);
}
