#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"BPF_TEST_RUN FAIL %d errno=%d: %s\n",__LINE__,errno,#c); exit(1); } } while (0)
#define ERROR(c,e) do { errno=0; CHECK((c)==-1 && errno==(e)); } while (0)
/* Linux UAPI layouts, independently of the kernel implementation. */
struct insn { uint8_t code,regs; int16_t off; int32_t imm; };
union attr {
    struct { uint32_t type,count; uint64_t insns,license; uint32_t log_level,log_size; uint64_t log; } prog;
    struct { uint32_t fd,retval,data_size_in,data_size_out; uint64_t data_in,data_out;
        uint32_t repeat,duration,ctx_size_in,ctx_size_out; uint64_t ctx_in,ctx_out;
        uint32_t flags,cpu,batch_size; } test;
    uint64_t padding[32];
};
#define I(c,d,s,o,i) {c,(d)|((s)<<4),o,i}
#define EXIT I(0x95,0,0,0,0)
static long call(unsigned cmd,void *a,unsigned size) { return syscall(SYS_bpf,cmd,a,size); }
static int load(unsigned type,struct insn *code,unsigned count)
{
    char log[4096]={0};
    union attr a;memset(&a,0,sizeof(a));
    a.prog.type=type;a.prog.count=count;a.prog.insns=(uintptr_t)code;
    a.prog.license=(uintptr_t)"GPL";a.prog.log_level=1;a.prog.log_size=sizeof(log);a.prog.log=(uintptr_t)log;
    int fd=call(5,&a,sizeof(a));
    if(fd<0)fprintf(stderr,"load errno=%d %s\n",errno,log);
    CHECK(fd>=0);return fd;
}
static void raw_context(void)
{
    struct insn read[]={I(0x79,0,1,8,0),EXIT};
    int fd=load(17,read,2);uint64_t ctx[12]={0,0x12345678};
    union attr a;memset(&a,0,sizeof(a));a.test.fd=fd;a.test.ctx_in=(uintptr_t)ctx;a.test.ctx_size_in=8;
    ERROR(call(10,&a,sizeof(a)),EINVAL);
    a.test.ctx_size_in=0;ERROR(call(10,&a,sizeof(a)),EINVAL);
    a.test.ctx_size_in=16;CHECK(!call(10,&a,sizeof(a)) && a.test.retval==ctx[1]);
    a.test.ctx_size_in=17;CHECK(!call(10,&a,sizeof(a)) && a.test.retval==ctx[1]);
    a.test.ctx_size_in=96;CHECK(!call(10,&a,sizeof(a)) && a.test.retval==ctx[1]);
    a.test.ctx_size_in=97;ERROR(call(10,&a,sizeof(a)),EINVAL);
    a.test.ctx_size_in=16;a.test.ctx_in=1;ERROR(call(10,&a,sizeof(a)),EFAULT);
    CHECK(!close(fd));puts("BPF_TEST_RUN CASE raw_context PASS");
}
static void output_fields(void)
{
    struct insn constant[]={I(0xb7,0,0,0,73),EXIT};
    int fd=load(17,constant,2);union attr a;memset(&a,0,sizeof(a));a.test.fd=fd;
    CHECK(!call(10,&a,sizeof(a)) && a.test.retval==73);
    ((unsigned char *)&a)[76]=1;ERROR(call(10,&a,sizeof(a)),EINVAL);
    ((unsigned char *)&a)[76]=0;
    a.test.duration=1;ERROR(call(10,&a,sizeof(a)),EINVAL);
    size_t page=sysconf(_SC_PAGESIZE);
    unsigned char *mem=mmap(NULL,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mem!=MAP_FAILED);
    CHECK(!mprotect(mem+2*page,page,PROT_NONE));
    uint32_t *short_attr=(void *)(mem+2*page-8);short_attr[0]=fd;short_attr[1]=0;
    CHECK(!call(10,short_attr,4) && short_attr[1]==73);
    /* The fd is readable-only; the adjacent retval is writable. */
    uint32_t *split=(void *)(mem+page-4);split[0]=fd;split[1]=0;
    CHECK(!mprotect(mem,page,PROT_READ));
    CHECK(!call(10,split,4) && split[1]==73);
    CHECK(!mprotect(mem+page,page,PROT_READ));ERROR(call(10,split,4),EFAULT);
    CHECK(!munmap(mem,3*page));CHECK(!close(fd));
    puts("BPF_TEST_RUN CASE output_fields PASS");
}
static uint32_t packet_run(struct insn *code,unsigned count,unsigned char *data,unsigned size)
{
    int fd=load(1,code,count);union attr a;memset(&a,0,sizeof(a));a.test.fd=fd;a.test.data_in=(uintptr_t)data;a.test.data_size_in=size;
    CHECK(!call(10,&a,sizeof(a)) && a.test.data_size_out==size);
    CHECK(!close(fd));return a.test.retval;
}
static void packets(void)
{
    unsigned char data[54]={0},out[80];data[12]=0x08;data[13]=0x00;data[14]=0x45;
    struct insn ctx[]={I(0x61,0,1,0,0),EXIT};
    CHECK(packet_run(ctx,2,data,34)==20);
    ctx[0].off=16;CHECK(packet_run(ctx,2,data,34)==8); /* wire-order ETH_P_IP */
    ctx[0].off=36;CHECK(packet_run(ctx,2,data,34)==0); /* no ingress device */
    ctx[0].off=40;CHECK(packet_run(ctx,2,data,34)==1); /* loopback device */
    ctx[0].off=4;CHECK(packet_run(ctx,2,data,34)==0);
    memset(data,255,6);CHECK(packet_run(ctx,2,data,34)==2); /* loopback broadcast address is zero */
    memset(data,0,6);data[0]=2;CHECK(packet_run(ctx,2,data,34)==3);memset(data,0,6);
    ctx[0].off=16;data[12]=0;data[13]=4;CHECK(packet_run(ctx,2,data,34)==0x0400);
    data[14]=255;data[15]=255;CHECK(packet_run(ctx,2,data,34)==0x0100);
    data[12]=8;data[13]=0;data[14]=0x45;data[15]=0;
    struct insn abs[]={I(0xbf,6,1,0,0),I(0x30,0,0,0,0),EXIT};
    CHECK(packet_run(abs,3,data,34)==0x45);
    abs[1].imm=-0x100000;CHECK(packet_run(abs,3,data,34)==0x45);
    abs[1].imm=-0x200000+12;CHECK(packet_run(abs,3,data,34)==8);
    abs[1].imm=20;CHECK(packet_run(abs,3,data,34)==0);
    int fd=load(1,ctx,2);union attr a;memset(&a,0,sizeof(a));a.test.fd=fd;a.test.data_in=(uintptr_t)data;
    a.test.data_size_in=34;a.test.data_out=(uintptr_t)out;
    memset(out,0xa5,sizeof(out));CHECK(!call(10,&a,sizeof(a)) && a.test.data_size_out==34);
    CHECK(!memcmp(out,(unsigned char[14]){0},14) && !memcmp(out+14,data+14,20) && out[34]==0xa5);
    memset(out,0xa5,sizeof(out));a.test.data_size_out=16;
    ERROR(call(10,&a,sizeof(a)),ENOSPC);
    CHECK(a.test.data_size_out==34 && out[14]==0x45 && out[16]==0xa5);
    a.test.data_out=0;a.test.data_size_out=1;ERROR(call(10,&a,sizeof(a)),ENOSPC);
    a.test.data_size_out=0;a.test.data_size_in=33;ERROR(call(10,&a,sizeof(a)),EINVAL);
    data[12]=0x86;data[13]=0xdd;a.test.data_size_in=53;ERROR(call(10,&a,sizeof(a)),EINVAL);
    a.test.data_size_in=54;CHECK(!call(10,&a,sizeof(a)));
    data[12]=0x88;data[13]=0xb5;a.test.data_size_in=14;CHECK(!call(10,&a,sizeof(a)));
    a.test.data_size_in=13;ERROR(call(10,&a,sizeof(a)),EINVAL);
    /* A short socket attribute has three output words; later bytes are untouched. */
    memset(&a,0,sizeof(a));a.test.fd=fd;a.test.data_in=(uintptr_t)data;a.test.data_size_in=14;
    memset((unsigned char *)&a+40,0xa5,sizeof(a)-40);
    CHECK(!call(10,&a,40));for(unsigned i=40;i<sizeof(a);i++)CHECK(((unsigned char *)&a)[i]==0xa5);
    CHECK(!close(fd));
    struct insn cb[]={I(0x61,0,1,48,0),I(0x04,0,0,0,1),I(0x63,1,0,48,0),EXIT};
    fd=load(1,cb,4);memset(&a,0,sizeof(a));a.test.fd=fd;
    a.test.data_in=(uintptr_t)data;a.test.data_size_in=14;a.test.repeat=3;
    CHECK(!call(10,&a,sizeof(a)) && a.test.retval==3);
    CHECK(!close(fd));puts("BPF_TEST_RUN CASE packets PASS");
}
int main(void)
{
    raw_context();output_fields();packets();puts("BPF_TEST_RUN: PASS");return 0;
}
