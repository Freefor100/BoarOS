#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"BPF ABI FAIL %d errno=%d: %s\n",__LINE__,errno,#c); exit(1); } } while (0)
#define ERROR(c,e) do { errno=0; CHECK((c)==-1 && errno==(e)); } while (0)
/* The fixture uses Linux UAPI layouts, independently of kernel-private types. */
union attr {
    struct { uint32_t type,key_size,value_size,max_entries,flags,inner,numa; char name[16];
        uint32_t ifindex,btf_fd,key_type,value_type; } map;
    struct { uint32_t fd,pad; uint64_t key,value,flags; } elem;
    struct { uint32_t type,count; uint64_t insns,license; uint32_t log_level,log_size; uint64_t log; uint32_t version,flags; char name[16]; } prog;
    struct { uint32_t fd,length; uint64_t data; } info;
    struct { uint32_t id,next_id,flags; } id;
    struct { uint64_t in,out,keys,values; uint32_t count,fd; uint64_t element_flags,flags; } batch;
    uint64_t bytes[32];
};
struct insn { uint8_t code,regs; int16_t off; int32_t imm; };
static long call(unsigned command,union attr *a) { return syscall(280,command,a,sizeof(*a)); }
static void policy(void)
{
    const char *path="/proc/sys/kernel/unprivileged_bpf_disabled";
    int fd=open(path,O_RDONLY);char text[16]={0};CHECK(fd>=0);
    CHECK(read(fd,text,sizeof(text))==2 && !strcmp(text,"2\n") && !close(fd));
    fd=open(path,O_WRONLY);CHECK(fd>=0);
    ERROR(write(fd,"3",1),EINVAL);CHECK(write(fd,"2",1)==1 && !close(fd));
    puts("BPF ABI CASE policy PASS");
}
static void arrays(void)
{
    union attr a={.map={.type=2,.key_size=4,.value_size=8,.max_entries=4,.name="abi_array"}};
    int fd=call(0,&a); CHECK(fd>=0 && fcntl(fd,F_GETFD)==FD_CLOEXEC);
    uint32_t key=1; uint64_t value=UINT64_C(0x1020304050607080),got=0;
    a=(union attr){.elem={.fd=fd,.key=(uintptr_t)&key,.value=(uintptr_t)&value}};
    CHECK(call(2,&a)==0);
    a.elem.value=(uintptr_t)&got;CHECK(call(1,&a)==0 && got==value);
    a.elem.flags=1;ERROR(call(2,&a),EEXIST);a.elem.flags=0;
    key=4;ERROR(call(1,&a),ENOENT);ERROR(call(2,&a),E2BIG);
    key=1;a.elem.key=1;ERROR(call(1,&a),EFAULT);a.elem.key=(uintptr_t)&key;
    int copy=dup(fd);CHECK(copy>=0 && !close(fd));a.elem.fd=copy;
    got=0;CHECK(call(1,&a)==0 && got==value);
    pid_t child=fork();CHECK(child>=0);
    if (!child) {got=0;CHECK(call(1,&a)==0 && got==value);_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!close(copy));ERROR(call(1,&a),EBADF);
    puts("BPF ABI CASE array PASS");
}
static void programs(void)
{
    struct insn code[]={{0xb7,0,0,-1},{0x95,0,0,0}};
    char log[4096]={0};
    union attr a={.prog={.type=1,.count=2,.insns=(uintptr_t)code,.license=(uintptr_t)"GPL",
        .log_level=1,.log_size=sizeof(log),.log=(uintptr_t)log,.name="abi_filter"}};
    int fd=call(5,&a);
    if(fd<0)fprintf(stderr,"BPF ABI load errno=%d log=%s\n",errno,log);
    CHECK(fd>=0 && log[0]);
    int sock=socket(AF_INET,SOCK_DGRAM,0);CHECK(sock>=0);
    CHECK(!setsockopt(sock,SOL_SOCKET,50,&fd,sizeof(fd)) && !close(fd));
    CHECK(!setsockopt(sock,SOL_SOCKET,27,&fd,sizeof(fd)));
    ERROR(setsockopt(sock,SOL_SOCKET,27,&fd,sizeof(fd)),ENOENT);
    CHECK(!close(sock));
    code[0]=(struct insn){0xbf,0x20,0,0};
    memset(log,0,sizeof(log));ERROR(call(5,&a),EACCES);CHECK(log[0]);
    puts("BPF ABI CASE programs PASS");
}
static void map_inputs(void)
{
    /* With no BTF types, Linux ignores the ELF object's BTF FD. */
    union attr a;memset(&a,0,sizeof(a));
    a.bytes[0]=1;((unsigned char *)&a)[20]=1;
    ERROR(call(6,&a),EINVAL);ERROR(call(7,&a),EINVAL);memset(&a,0,sizeof(a));
    a.map.type=27;a.map.max_entries=(uint32_t)sysconf(_SC_PAGESIZE);a.map.btf_fd=UINT32_MAX;
    int fd=call(0,&a);CHECK(fd>=0 && !close(fd));
    memset(&a,0,sizeof(a));a.map.type=1;a.map.key_size=4;a.map.value_size=8;a.map.max_entries=4;
    fd=call(0,&a);CHECK(fd>=0);
    uint32_t key=7;uint64_t value=11,got=0,replacement=99;
    memset(&a,0,sizeof(a));a.elem.fd=fd;a.elem.key=(uintptr_t)&key;a.elem.value=(uintptr_t)&value;
    CHECK(!call(2,&a));a.elem.value=(uintptr_t)&got;
    for(unsigned i=0;i<2;i++){
        a.elem.flags=i?4:UINT64_C(1)<<63;ERROR(call(21,&a),EINVAL);
        a.elem.flags=0;CHECK(!call(1,&a) && got==value);
    }
    long page=sysconf(_SC_PAGESIZE);
    union attr *readonly=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(readonly!=MAP_FAILED);
    for(unsigned command=26;command<=27;command++){
        memset(&a,0,sizeof(a));a.batch.fd=fd;a.batch.count=1;a.batch.keys=(uintptr_t)&key;
        a.batch.values=(uintptr_t)&replacement;a.batch.element_flags=UINT64_C(1)<<63;
        ERROR(call(command,&a),EINVAL);
        a.batch.element_flags=0;memcpy(readonly,&a,sizeof(a));CHECK(!mprotect(readonly,page,PROT_READ));
        ERROR(call(command,readonly),EFAULT);CHECK(!mprotect(readonly,page,PROT_READ|PROT_WRITE));
        memset(&a,0,sizeof(a));a.elem.fd=fd;a.elem.key=(uintptr_t)&key;a.elem.value=(uintptr_t)&got;
        CHECK(!call(1,&a) && got==value);
    }
    CHECK(!munmap(readonly,page) && !close(fd));
    puts("BPF ABI CASE map inputs PASS");
}
static void writeonly_program_map(void)
{
    union attr a;memset(&a,0,sizeof(a));
    a.map.type=2;a.map.key_size=4;a.map.value_size=8;a.map.max_entries=1;a.map.flags=256;
    int map=call(0,&a);CHECK(map>=0);
    struct insn code[]={
        {0x18,0x11,0,map},{0,0,0,0},{0xbf,0xa2,0,0},{0x07,2,0,-4},
        {0x62,10,-4,0},{0x85,0,0,1},{0x15,0,1,0},{0x7a,0,0,73},
        {0xb7,0,0,1},{0x95,0,0,0}};
    char log[4096]={0};memset(&a,0,sizeof(a));a.prog.type=1;a.prog.count=sizeof(code)/sizeof(*code);
    a.prog.insns=(uintptr_t)code;a.prog.license=(uintptr_t)"GPL";
    a.prog.log=(uintptr_t)log;a.prog.log_size=sizeof(log);a.prog.log_level=1;
    int program=call(5,&a);if(program<0)fprintf(stderr,"writeonly map load: %s\n",log);
    CHECK(program>=0);CHECK(!close(program));
    code[7]=(struct insn){0x79,0,0,0};memset(log,0,sizeof(log));
    ERROR(call(5,&a),EACCES);CHECK(log[0]);
    CHECK(!close(map));puts("BPF ABI CASE writeonly program map PASS");
}
static void capability_separation(void)
{
    int event=open("/sys/kernel/tracing/events/raw_syscalls/sys_enter/id",O_RDONLY);
    char text[32]={0};CHECK(event>=0 && read(event,text,sizeof(text)-1)>0 && !close(event));
    uint64_t trace_id=strtoull(text,NULL,10);CHECK(trace_id);
    union attr a;memset(&a,0,sizeof(a));a.map.type=2;a.map.key_size=4;a.map.value_size=8;a.map.max_entries=1;
    int inherited=call(0,&a);CHECK(inherited>=0);
    for(unsigned selected=0;selected<4;selected++){
        pid_t child=fork();CHECK(child>=0);
        if(!child){
            struct {uint32_t version;int32_t pid;} header={0x20080522,0};
            struct {uint32_t effective,permitted,inheritable;} caps[2]={{0}};
            uint32_t high=(selected&1?1U<<(39-32):0)|(selected&2?1U<<(38-32):0);
            caps[1].effective=caps[1].permitted=high;
            CHECK(!syscall(SYS_capset,&header,caps));
            uint32_t key=0;uint64_t value=99;
            memset(&a,0,sizeof(a));a.elem.fd=inherited;a.elem.key=(uintptr_t)&key;a.elem.value=(uintptr_t)&value;
            CHECK(!call(1,&a) && !value);
            memset(&a,0,sizeof(a));a.map.type=2;a.map.key_size=4;a.map.value_size=8;a.map.max_entries=1;
            if(selected&1){int fd=call(0,&a);CHECK(fd>=0 && !close(fd));}
            else ERROR(call(0,&a),EPERM);
            struct insn code[]={{0xb7,0,0,0},{0x95,0,0,0}};
            memset(&a,0,sizeof(a));a.prog.type=2;a.prog.count=2;
            a.prog.insns=(uintptr_t)code;a.prog.license=(uintptr_t)"GPL";
            if(selected==3){int fd=call(5,&a);CHECK(fd>=0 && !close(fd));}
            else ERROR(call(5,&a),EPERM);
            struct {uint32_t type,size;uint64_t config,period,sample_type,read_format,flags;
                uint32_t wakeup,bp_type;uint64_t config1;} perf={.type=2,.size=64,.config=trace_id,.period=1,.flags=1};
            if(selected&2){int fd=syscall(SYS_perf_event_open,&perf,0,-1,-1,8);CHECK(fd>=0 && !close(fd));}
            else ERROR(syscall(SYS_perf_event_open,&perf,0,-1,-1,8),EACCES);
            CHECK(!close(inherited));_exit(0);
        }
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    CHECK(!close(inherited));puts("BPF ABI CASE CAP_BPF-PERFMON separation PASS");
}
int main(void)
{
    policy();arrays();programs();map_inputs();writeonly_program_map();capability_separation();
    puts("BPF ABI PASS");
    return 0;
}
