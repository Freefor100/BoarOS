#include "abi.h"
static const char runtime_path[] = "/rt-proc/sys/kernel/sched_rt_runtime_us";
static const char period_path[] = "/rt-proc/sys/kernel/sched_rt_period_us";
static void write_case(const char *name, const char *path, const char *value, usize n)
{
    long fd=abi_open(path,2);
    abi_require(fd>=0);
    long ret=SC3(64,fd,value,n);
    abi_record(name,ret,-1,abi_offset(fd),0,0,0);
    abi_require(SC1(57,fd)==0);
}
void abi_rt_controls_cases(void)
{
    abi_require(SC3(34,-100,"/rt-proc",0755)==0 && SC5(40,"proc","/rt-proc","proc",0,0)==0);
    long fd=abi_open(runtime_path,2);
    abi_record("rtctl.open",fd<0?fd:0,-1,-1,0,0,0);
    if(fd<0)return;
    /* Both kernels start the comparison from the same explicit settings. */
    abi_require(SC3(64,fd,"-1",2)==2 && SC1(57,fd)==0);
    write_case("rtctl.period",period_path,"1000000",7);
    write_case("rtctl.runtime",runtime_path,"950000",6);
    write_case("rtctl.period-zero",period_path,"0",1);
    write_case("rtctl.period-negative",period_path,"-1",2);
    write_case("rtctl.period-too-small",period_path,"949999",6);
    write_case("rtctl.runtime-too-large",runtime_path,"1000001",7);
    write_case("rtctl.runtime-negative",runtime_path,"-2",2);
    write_case("rtctl.overflow",runtime_path,"2147483648",10);
    write_case("rtctl.plus",runtime_path,"+900000",7);
    write_case("rtctl.junk",runtime_path,"900000x",7);
    write_case("rtctl.spaces",runtime_path," \t\n",3);
    write_case("rtctl.hex",runtime_path,"0xdbba0",7);
    write_case("rtctl.octal",runtime_path,"03335640",8);
    write_case("rtctl.trailing-token",runtime_path,"950000 12",9);
    fd=abi_open(runtime_path,2);abi_require(fd>=0);
    char buf[32];
    long r=SC3(63,fd,buf,2);
    abi_record("rtctl.short-read",r,-1,abi_offset(fd),0,buf,r>0?(usize)r:0);
    abi_record("rtctl.short-eof",SC3(63,fd,buf,sizeof(buf)),-1,abi_offset(fd),0,0,0);
    abi_record("rtctl.nonzero-write",SC3(64,fd,"-1",2),-1,abi_offset(fd),0,0,0);
    abi_require(SC3(62,fd,0,0)==0);
    r=SC3(63,fd,buf,sizeof(buf));
    abi_record("rtctl.seek-refresh",r,-1,abi_offset(fd),0,buf,r>0?(usize)r:0);
    abi_record("rtctl.pread-offset",SC4(67,fd,buf,sizeof(buf),1),-1,-1,0,0,0);
    abi_record("rtctl.pwrite-offset",SC4(68,fd,"-1",2,1),-1,-1,0,0,0);
    abi_require(SC3(62,fd,0,0)==0);
    long map=SC6(222,0,(2 * ABI_PAGE_SIZE),3,0x22,-1,0);
    abi_require(map>=0 && SC3(226,map+ABI_PAGE_SIZE,ABI_PAGE_SIZE,0)==0);
    *(char *)(map+4095)='1';
    abi_record("rtctl.write-fault",SC3(64,fd,map+4095,2),-1,abi_offset(fd),0,0,0);
    abi_record("rtctl.read-fault",SC3(63,fd,map+4095,2),-1,abi_offset(fd),0,0,0);
    abi_require(SC3(62,fd,0,0)==0);
    struct { const void *base; usize length; } iov[2]={{"95",2},{"0000",4}};
    abi_record("rtctl.writev",SC3(66,fd,iov,2),-1,abi_offset(fd),0,0,0);
    abi_require(SC3(62,fd,0,0)==0);
    iov[0].base="0";iov[0].length=1;
    iov[1].base=(const void *)(map+ABI_PAGE_SIZE);iov[1].length=1;
    abi_record("rtctl.writev-fault",SC3(66,fd,iov,2),-1,abi_offset(fd),0,0,0);
    r=SC3(63,fd,buf,sizeof(buf));
    abi_record("rtctl.writev-fault-state",r,-1,abi_offset(fd),0,buf,r>0?(usize)r:0);
    abi_record("rtctl.nonzero-write-fault",SC3(64,fd,map+4095,2),-1,abi_offset(fd),0,0,0);
    abi_record("rtctl.eof-fault-buffer",SC3(63,fd,map+ABI_PAGE_SIZE,1),-1,abi_offset(fd),0,0,0);
    long other=abi_open(runtime_path,1);abi_require(other>=0);
    abi_record("rtctl.read-wronly",SC3(63,other,buf,sizeof(buf)),-1,-1,0,0,0);
    abi_require(SC3(64,other,"900000",6)==6 && SC1(57,other)==0);
    abi_require(SC3(62,fd,0,0)==0);
    r=SC3(63,fd,buf,sizeof(buf));
    abi_record("rtctl.dynamic-read",r,-1,abi_offset(fd),0,buf,r>0?(usize)r:0);
    write_case("rtctl.restore-runtime",runtime_path,"950000",6);
    abi_require(SC3(34,-100,"/proc-ro",0755)==0 && SC5(40,"proc","/proc-ro","proc",1,0)==0);
    abi_record("rtctl.readonly-open",abi_open("/proc-ro/sys/kernel/sched_rt_runtime_us",2),-1,-1,0,0,0);
    abi_require(SC2(39,"/proc-ro",0)==0);
    abi_record("rtctl.seek-end",SC3(62,fd,0,2),-1,abi_offset(fd),0,0,0);
    abi_record("rtctl.seek-data",SC3(62,fd,0,3),-1,abi_offset(fd),0,0,0);
    abi_record("rtctl.seek-hole",SC3(62,fd,0,4),-1,abi_offset(fd),0,0,0);
    abi_record("rtctl.seek-hole-negative",SC3(62,fd,-1,4),-1,abi_offset(fd),0,0,0);
    abi_require(SC1(57,fd)==0 && SC2(215,map,(2 * ABI_PAGE_SIZE))==0 && SC2(39,"/rt-proc",0)==0);
}
