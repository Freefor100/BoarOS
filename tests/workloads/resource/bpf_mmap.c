#define _GNU_SOURCE
#include <errno.h>
#include <linux/bpf.h>
#include <linux/futex.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"BPF MMAP FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}}while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1 && errno==(e));}while(0)
static size_t page;
static unsigned discard_mismatches;
static int bpf(unsigned cmd,union bpf_attr *a){return syscall(SYS_bpf,cmd,a,sizeof(*a));}
static int array(unsigned flags)
{
 union bpf_attr a={.map_type=BPF_MAP_TYPE_ARRAY,.key_size=4,.value_size=8,.max_entries=page/8,.map_flags=BPF_F_MMAPABLE|flags};
 int fd=bpf(BPF_MAP_CREATE,&a);CHECK(fd>=0);return fd;
}
static int freeze(int fd){union bpf_attr a={.map_fd=fd};return bpf(BPF_MAP_FREEZE,&a);}
static void put(int fd,unsigned key,uint64_t value)
{union bpf_attr a={.map_fd=fd,.key=(uintptr_t)&key,.value=(uintptr_t)&value};CHECK(!bpf(BPF_MAP_UPDATE_ELEM,&a));}
static uint64_t get(int fd,unsigned key)
{uint64_t value=0;union bpf_attr a={.map_fd=fd,.key=(uintptr_t)&key,.value=(uintptr_t)&value};CHECK(!bpf(BPF_MAP_LOOKUP_ELEM,&a));return value;}
static void discard_fault(void *address,size_t length)
{
 pid_t child=fork();CHECK(child>=0);
 if(!child){CHECK(!madvise(address,length,MADV_DONTNEED));volatile uint64_t value=*(volatile uint64_t *)address;(void)value;_exit(99);}
 int status;CHECK(waitpid(child,&status,0)==child);
 if(!WIFSIGNALED(status) || WTERMSIG(status)!=SIGBUS){printf("BPF MMAP DONTNEED mismatch status=%d expected-signal=%d\n",status,SIGBUS);discard_mismatches++;}
}
static void reject_object_duplication(void *address,size_t length)
{
 ERROR(mremap(address,0,length,0)==MAP_FAILED?-1:0,EFAULT);
 void *target=mmap(NULL,length,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(target!=MAP_FAILED);
 for(unsigned fixed=0;fixed<=MREMAP_FIXED;fixed+=MREMAP_FIXED){
  ERROR(mremap(address,0,length,MREMAP_MAYMOVE|fixed,target)==MAP_FAILED?-1:0,EFAULT);
  ERROR(mremap(address,length,length,MREMAP_MAYMOVE|MREMAP_DONTUNMAP|fixed,target)==MAP_FAILED?-1:0,EINVAL);
 }
 CHECK(!munmap(target,length));
}
static void array_permissions(void)
{
 int fd=array(0);uint64_t *rw=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(rw!=MAP_FAILED);
 unsigned char resident=0;CHECK(!mincore(rw,page,&resident) && (resident&1));
 uint64_t *none=mmap(NULL,page,PROT_NONE,MAP_SHARED,fd,0);CHECK(none!=MAP_FAILED);
 resident=0;CHECK(!mincore(none,page,&resident) && (resident&1));
 CHECK(!mprotect(none,page,PROT_READ));discard_fault(none,page);CHECK(none[0]==0);CHECK(!munmap(none,page));
 put(fd,0,0x123456789abcdef0ULL);CHECK(rw[0]==0x123456789abcdef0ULL);rw[1]=0x1020304050607080ULL;CHECK(get(fd,1)==rw[1]);
 ERROR(mmap(NULL,page,PROT_READ,MAP_SHARED,fd,page)==MAP_FAILED?-1:0,EINVAL);
 ERROR(mmap(NULL,page,PROT_READ,MAP_PRIVATE,fd,0)==MAP_FAILED?-1:0,EINVAL);
 ERROR(mmap(NULL,page,PROT_READ|PROT_EXEC,MAP_SHARED,fd,0)==MAP_FAILED?-1:0,EPERM);
 uint64_t *ro=mmap(NULL,page,PROT_READ,MAP_SHARED,fd,0);CHECK(ro!=MAP_FAILED && ro[1]==rw[1]);
 ERROR(mprotect(ro,page,PROT_READ|PROT_WRITE),EACCES);ERROR(mprotect(rw,page,PROT_READ|PROT_EXEC),EACCES);
 ERROR(freeze(fd),EBUSY);CHECK(!mprotect(rw,page,PROT_READ));ERROR(freeze(fd),EBUSY);
 CHECK(!munmap(rw,page) && !freeze(fd));CHECK(ro[0]==0x123456789abcdef0ULL);
 ERROR(mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED?-1:0,EPERM);
 CHECK(!close(fd));CHECK(ro[1]==0x1020304050607080ULL);discard_fault(ro,page);CHECK(!munmap(ro,page));
 fd=array(BPF_F_RDONLY_PROG);ERROR(mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0)==MAP_FAILED?-1:0,EACCES);CHECK(!close(fd));
 puts("BPF MMAP CASE array-permissions-freeze PASS");
}
static void fork_lifetime(void)
{
 int fd=array(0);uint64_t *p=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);
 int ready[2],go[2];CHECK(!pipe(ready) && !pipe(go));pid_t child=fork();CHECK(child>=0);
 if(!child){close(ready[0]);close(go[1]);CHECK(!close(fd));p[0]=99;CHECK(!mprotect(p,page,PROT_READ));CHECK(write(ready[1],"r",1)==1);char byte;CHECK(read(go[0],&byte,1)==1);CHECK(p[0]==101);CHECK(!munmap(p,page));CHECK(write(ready[1],"u",1)==1);_exit(0);}
 close(ready[1]);close(go[0]);char byte;CHECK(read(ready[0],&byte,1)==1 && p[0]==99);p[0]=101;
 CHECK(!munmap(p,page));ERROR(freeze(fd),EBUSY);CHECK(write(go[1],"g",1)==1);CHECK(read(ready[0],&byte,1)==1);CHECK(!freeze(fd));
 int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));CHECK(get(fd,0)==101);CHECK(!close(fd));close(ready[0]);close(go[1]);
 fd=array(0);p=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(p!=MAP_FAILED);CHECK(!close(fd));
 /* First user access is after final FD close; mmap already owns the pages. */
 CHECK(p[0]==0);p[0]=37;discard_fault(p,page);CHECK(p[0]==37);
 ERROR(syscall(SYS_futex,p,FUTEX_WAKE,1,NULL,NULL,0),EFAULT);
 ERROR(mremap(p,page,2*page,MREMAP_MAYMOVE)==MAP_FAILED?-1:0,EFAULT);
 reject_object_duplication(p,page);CHECK(p[0]==37);
 CHECK(!munmap(p,page));puts("BPF MMAP CASE fork-first-access-lifetime PASS");
}

struct remote {int fd,cpu,index;pthread_barrier_t *barrier;};
static void meet(pthread_barrier_t *barrier)
{int result=pthread_barrier_wait(barrier);CHECK(!result || result==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *remote_mapping(void *argument)
{
 struct remote *r=argument;cpu_set_t set;CPU_ZERO(&set);CPU_SET(r->cpu,&set);CHECK(!sched_setaffinity(0,sizeof(set),&set));CHECK(sched_getcpu()==r->cpu);
 uint64_t *p=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,r->fd,0);CHECK(p!=MAP_FAILED);p[r->index]=200+r->index;
 meet(r->barrier);meet(r->barrier);CHECK(!mprotect(p,page,PROT_READ));
 meet(r->barrier);meet(r->barrier);CHECK(p[r->index]==(uint64_t)(200+r->index));CHECK(!munmap(p,page));
 meet(r->barrier);return NULL;
}
static void remote_permissions(void)
{
 cpu_set_t available;CHECK(!sched_getaffinity(0,sizeof(available),&available));
 struct remote args[8];pthread_t threads[8];pthread_barrier_t barrier;int count=0,fd=array(0);
 for(int cpu=0;cpu<CPU_SETSIZE && count<8;cpu++)if(CPU_ISSET(cpu,&available)){args[count]=(struct remote){fd,cpu,count,&barrier};count++;}
 CHECK(count && !pthread_barrier_init(&barrier,NULL,count+1));
 for(int i=0;i<count;i++)CHECK(!pthread_create(&threads[i],NULL,remote_mapping,&args[i]));
 meet(&barrier);for(int i=0;i<count;i++)CHECK(get(fd,i)==(uint64_t)(200+i));meet(&barrier);
 meet(&barrier);ERROR(freeze(fd),EBUSY);meet(&barrier);meet(&barrier);CHECK(!freeze(fd));
 for(int i=0;i<count;i++)CHECK(!pthread_join(threads[i],NULL));
 CHECK(!pthread_barrier_destroy(&barrier) && !close(fd));printf("BPF MMAP CASE remote-mprotect-unmap cpus=%d PASS\n",count);
}

static void ring_alias(void)
{
 union bpf_attr a={.map_type=BPF_MAP_TYPE_RINGBUF,.max_entries=page};int fd=bpf(BPF_MAP_CREATE,&a);CHECK(fd>=0);
 uint64_t *consumer=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);CHECK(consumer!=MAP_FAILED);
 uint64_t *producer=mmap(NULL,3*page,PROT_READ,MAP_SHARED,fd,page);CHECK(producer!=MAP_FAILED);
 ERROR(mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,page)==MAP_FAILED?-1:0,EPERM);
 ERROR(mprotect(producer,page,PROT_READ|PROT_WRITE),EACCES);
 reject_object_duplication(consumer,page);reject_object_duplication(producer,3*page);
 /* Produce through the real ringbuf_output helper, so duplicate data windows
  * are tested against populated bytes rather than equal zero-filled pages. */
 struct bpf_insn code[]={
  {.code=0x18,.dst_reg=1,.src_reg=BPF_PSEUDO_MAP_FD,.imm=fd},{0},
  {.code=0x62,.dst_reg=10,.off=-8,.imm=0x12345678},
  {.code=0xbf,.dst_reg=2,.src_reg=10},{.code=0x07,.dst_reg=2,.imm=-8},
  {.code=0xb7,.dst_reg=3,.imm=4},{.code=0xb7,.dst_reg=4},
  {.code=0x85,.imm=BPF_FUNC_ringbuf_output},{.code=0xb7,.imm=-1},{.code=0x95}};
 a=(union bpf_attr){.prog_type=BPF_PROG_TYPE_SOCKET_FILTER,.insn_cnt=sizeof(code)/sizeof(code[0]),.insns=(uintptr_t)code,.license=(uintptr_t)"GPL"};
 int prog=bpf(BPF_PROG_LOAD,&a);CHECK(prog>=0);char input[64]={0};a=(union bpf_attr){0};a.test.prog_fd=prog;a.test.data_in=(uintptr_t)input;a.test.data_size_in=sizeof(input);a.test.repeat=1;CHECK(!bpf(BPF_PROG_TEST_RUN,&a));
 CHECK(producer[0]==16);volatile uint32_t *data=(void *)((char *)producer+page);CHECK(data[0]==4 && data[2]==0x12345678);
 volatile uint32_t *alias=(void *)((char *)producer+2*page);CHECK(alias[0]==data[0] && alias[2]==data[2]);
 CHECK(!close(prog) && !close(fd));discard_fault(producer,3*page);CHECK(data[2]==0x12345678 && alias[2]==data[2]);consumer[0]=16;
 CHECK(!munmap(consumer,page) && !munmap(producer,3*page));puts("BPF MMAP CASE ring-duplicate-alias PASS");
}
int main(void)
{setvbuf(stdout,NULL,_IONBF,0);alarm(60);page=sysconf(_SC_PAGESIZE);CHECK(page==4096 || page==16384);printf("BPF MMAP PAGE %zu\n",page);array_permissions();fork_lifetime();remote_permissions();ring_alias();CHECK(!discard_mismatches);puts("BPF MMAP PASS");return 0;}
