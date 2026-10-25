#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){dprintf(2,"MADVISE FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);} }while(0)
#define ERROR(x,e) do {errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static size_t page;
static unsigned cases;
static unsigned char bss[3*16384] __attribute__((aligned(16384)));
static unsigned char *area(unsigned n,int flags)
{void *p=mmap(0,n*page,PROT_READ|PROT_WRITE,flags|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);return p;}
static void joined(pid_t c){int s;CHECK(waitpid(c,&s,0)==c&&WIFEXITED(s)&&!WEXITSTATUS(s));}
static void pass(const char *name){printf("MADVISE CASE %s PASS\n",name);cases++;}
static void token(int fd){CHECK(write(fd,"x",1)==1);}
static void expect(int fd){char x;CHECK(read(fd,&x,1)==1&&x=='x');}
static void abi(void)
{
    unsigned char *p=area(2,MAP_PRIVATE);
    for(int advice=0;advice<=4;advice++)CHECK(!madvise(p,0,advice));
    ERROR(madvise(p,0,-1),EINVAL);ERROR(madvise(p,0,0x7fffffff),EINVAL);
    ERROR(madvise(p+1,0,MADV_NORMAL),EINVAL);
    ERROR(madvise(p,SIZE_MAX,MADV_DONTNEED),EINVAL);
    CHECK(!madvise((void *)(UINTPTR_MAX&~(page-1)),0,MADV_DONTNEED));
    ERROR(madvise((void *)(UINTPTR_MAX&~(page-1)),page,MADV_DONTNEED),EINVAL);
    CHECK(!syscall(SYS_madvise,p,page,((uint64_t)1<<32)|MADV_NORMAL));
    for(int advice=0;advice<=2;advice++)CHECK(!madvise(p,2*page,advice));
    CHECK(!munmap(p,2*page));ERROR(madvise(p,page,MADV_NORMAL),ENOMEM);
    pass("abi-width-alignment-zero-overflow");
}
static void anonymous(void)
{
    unsigned char *p=area(3,MAP_PRIVATE);memset(p,73,3*page);
    CHECK(!madvise(p+page,1,MADV_DONTNEED));
    for(size_t i=0;i<page;i++)CHECK(!p[page+i]);
    CHECK(p[0]==73&&p[3*page-1]==73);
    CHECK(!mprotect(p,page,PROT_NONE)&&!madvise(p,page,MADV_DONTNEED));
    CHECK(!mprotect(p,page,PROT_READ|PROT_WRITE)&&!p[0]);
    CHECK(!munmap(p,3*page));pass("private-zero-rounding-protection");
}
static void cow(void)
{
    unsigned char *p=area(2,MAP_PRIVATE);memset(p,42,2*page);
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t c=fork();CHECK(c>=0);
    if(!c){token(from[1]);expect(to[0]);CHECK(p[0]==42&&p[page]==42);p[page]=51;token(from[1]);_exit(0);}
    expect(from[0]);CHECK(!madvise(p,2*page,MADV_DONTNEED)&&!p[0]&&!p[page]);
    token(to[1]);expect(from[0]);joined(c);CHECK(!p[page]);
    CHECK(!close(to[0])&&!close(to[1])&&!close(from[0])&&!close(from[1])&&!munmap(p,2*page));
    pass("private-cow-child-preserved");
}
static void shared(void)
{
    unsigned char *p=area(1,MAP_SHARED);p[0]=83;
    CHECK(!madvise(p,page,MADV_DONTNEED)&&p[0]==83);
    int id=shmget(IPC_PRIVATE,page,IPC_CREAT|0600);CHECK(id>=0);
    unsigned char *s=shmat(id,0,0);CHECK(s!=(void *)-1);s[0]=67;
    CHECK(!madvise(s,page,MADV_DONTNEED)&&s[0]==67);
    CHECK(!shmdt(s)&&!shmctl(id,IPC_RMID,0)&&!munmap(p,page));
    pass("shared-anonymous-and-sysv-content");
}
static void file_case(const char *path,const char *name)
{
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    unsigned char *bytes=malloc(3*page);CHECK(bytes);memset(bytes,'a',3*page);
    CHECK(write(fd,bytes,3*page)==(ssize_t)(3*page)&&!fsync(fd));free(bytes);
    unsigned char *s=mmap(0,3*page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    unsigned char *p=mmap(0,3*page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    CHECK(s!=MAP_FAILED&&p!=MAP_FAILED);p[0]='q';s[2*page]='s';
    CHECK(!madvise(p,page,MADV_DONTNEED)&&p[0]=='a');
    CHECK(!madvise(s+2*page,page,MADV_DONTNEED)&&s[2*page]=='s');
    CHECK(!msync(s,3*page,MS_SYNC)&&!fsync(fd));char ch=0;
    CHECK(pread(fd,&ch,1,2*page)==1&&ch=='s');
    off_t offset=lseek(fd,0,SEEK_CUR);CHECK(offset==(off_t)(3*page));
    CHECK(!madvise(p,3*page,MADV_WILLNEED)&&lseek(fd,0,SEEK_CUR)==offset);
    CHECK(!munmap(p,3*page)&&!munmap(s,3*page)&&!close(fd)&&!unlink(path));pass(name);
}
static void locked(void)
{
    unsigned char *p=area(3,MAP_PRIVATE);memset(p,19,3*page);
    CHECK(!mlock(p+page,page));ERROR(madvise(p,3*page,MADV_DONTNEED),EINVAL);
    CHECK(!p[0]&&p[page]==19&&p[2*page]==19);
    CHECK(!munlock(p+page,page)&&!syscall(SYS_mlock2,p+page,page,1));
    ERROR(madvise(p+page,page,MADV_DONTNEED),EINVAL);CHECK(p[page]==19);
    CHECK(!munlock(p+page,page)&&!munmap(p,3*page));pass("locked-and-onfault-reject-after-prefix");
}
static void hole(void)
{
    unsigned char *p=area(3,MAP_PRIVATE);p[0]=11;p[2*page]=17;
    CHECK(!munmap(p+page,page));ERROR(madvise(p,3*page,MADV_DONTNEED),ENOMEM);
    CHECK(!p[0]&&!p[2*page]);CHECK(!munmap(p,page)&&!munmap(p+2*page,page));
    pass("hole-keeps-mapped-side-effects");
}
static void willneed(void)
{
    unsigned char *p=area(2,MAP_PRIVATE);
    /* The fixed references and BoarOS have no swap: Linux returns EBADF for
     * private anonymous WILLNEED, while shmem/file mappings are accepted. */
    ERROR(madvise(p,page,MADV_WILLNEED),EBADF);CHECK(!munmap(p,2*page));
    ERROR(madvise(bss+page,page,MADV_WILLNEED),EBADF);
    p=area(1,MAP_SHARED);CHECK(!madvise(p,page,MADV_WILLNEED)&&!munmap(p,page));
    uintptr_t text=(uintptr_t)willneed&~(page-1);CHECK(!madvise((void *)text,page,MADV_WILLNEED));
    pass("willneed-no-swap-and-file-types");
}
struct remote {unsigned char *p;unsigned cpu;pthread_barrier_t *gate;};
static void gate(pthread_barrier_t *g){int e=pthread_barrier_wait(g);CHECK(!e||e==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *remote_run(void *arg)
{
    struct remote *r=arg;cpu_set_t set;CPU_ZERO(&set);CPU_SET(r->cpu,&set);CHECK(!sched_setaffinity(0,sizeof(set),&set));
    CHECK(r->p[r->cpu]==91);gate(r->gate);gate(r->gate);CHECK(!r->p[r->cpu]);
    r->p[r->cpu]=(unsigned char)(r->cpu+1);return 0;
}
static void remote(void)
{
    cpu_set_t set;CHECK(!sched_getaffinity(0,sizeof(set),&set));unsigned cpus=0;
    for(unsigned i=0;i<4;i++)if(CPU_ISSET(i,&set))cpus++;
    CHECK(cpus==1||cpus==2||cpus==4);
    unsigned char *p=area(1,MAP_PRIVATE);memset(p,91,page);
    pthread_barrier_t barrier;CHECK(!pthread_barrier_init(&barrier,0,cpus+1));
    pthread_t threads[4];struct remote args[4];
    for(unsigned i=0;i<cpus;i++){args[i]=(struct remote){p,i,&barrier};CHECK(!pthread_create(&threads[i],0,remote_run,&args[i]));}
    gate(&barrier);CHECK(!madvise(p,page,MADV_DONTNEED));gate(&barrier);
    for(unsigned i=0;i<cpus;i++){CHECK(!pthread_join(threads[i],0)&&p[i]==i+1);}
    CHECK(!pthread_barrier_destroy(&barrier)&&!munmap(p,page));pass("remote-tlb-refault-shared-mm");
}
static void fork_absent(void)
{
    unsigned char *p=area(4,MAP_PRIVATE);memset(p,61,4*page);
    CHECK(!madvise(p+page,2*page,MADV_DONTFORK));
    pid_t c=fork();CHECK(c>=0);
    if(!c){unsigned char v;ERROR(mincore(p+page,page,&v),ENOMEM);ERROR(mincore(p+2*page,page,&v),ENOMEM);CHECK(p[0]==61&&p[3*page]==61);_exit(0);}joined(c);
    CHECK(p[page]==61&&p[2*page]==61&&!madvise(p+page,2*page,MADV_DOFORK));
    c=fork();CHECK(c>=0);if(!c){CHECK(p[page]==61&&p[2*page]==61);p[page]=99;_exit(0);}joined(c);
    CHECK(p[page]==61&&!munmap(p,4*page));pass("dontfork-partial-dofork-cow");
}
static void fork_wipe(void)
{
    unsigned char *p=area(3,MAP_PRIVATE);memset(p,74,3*page);
    CHECK(!madvise(p+page,page,MADV_WIPEONFORK));
    pid_t c=fork();CHECK(c>=0);
    if(!c){CHECK(p[0]==74&&!p[page]&&p[2*page]==74);p[page]=81;
        pid_t g=fork();CHECK(g>=0);if(!g){CHECK(!p[page]);_exit(0);}joined(g);
        CHECK(p[page]==81&&!madvise(p+page,page,MADV_KEEPONFORK));
        g=fork();CHECK(g>=0);if(!g){CHECK(p[page]==81);_exit(0);}joined(g);_exit(0);}joined(c);
    CHECK(p[page]==74&&!madvise(p+page,page,MADV_DONTFORK));
    c=fork();CHECK(c>=0);if(!c){unsigned char v;ERROR(mincore(p+page,page,&v),ENOMEM);_exit(0);}joined(c);
    CHECK(!madvise(p+page,page,MADV_DOFORK));
    c=fork();CHECK(c>=0);if(!c){CHECK(!p[page]);_exit(0);}joined(c);
    CHECK(p[page]==74&&!munmap(p,3*page));pass("wipe-child-grandchild-and-keep");
}
static void fork_backing(void)
{
    unsigned char *p=area(1,MAP_SHARED);p[0]=23;
    ERROR(madvise(p,page,MADV_WIPEONFORK),EINVAL);CHECK(!madvise(p,page,MADV_KEEPONFORK));
    CHECK(!madvise(p,page,MADV_DONTFORK));pid_t c=fork();CHECK(c>=0);
    if(!c){unsigned char v;ERROR(mincore(p,page,&v),ENOMEM);_exit(0);}joined(c);
    CHECK(p[0]==23&&!munmap(p,page));
    int id=shmget(IPC_PRIVATE,page,IPC_CREAT|0600);CHECK(id>=0);p=shmat(id,0,0);CHECK(p!=(void*)-1);
    CHECK(!madvise(p,page,MADV_DONTFORK));c=fork();CHECK(c>=0);
    if(!c){struct shmid_ds ds;unsigned char v;ERROR(mincore(p,page,&v),ENOMEM);CHECK(!shmctl(id,IPC_STAT,&ds)&&ds.shm_nattch==1);_exit(0);}joined(c);
    CHECK(!shmdt(p)&&!shmctl(id,IPC_RMID,0));pass("dontfork-shared-and-shm-attachment");
}
static void fork_file(const char *path,const char *name)
{
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0&&!ftruncate(fd,2*page));
    unsigned char *p=mmap(0,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);CHECK(p!=MAP_FAILED);p[0]=35;p[page]=47;
    ERROR(madvise(p,page,MADV_WIPEONFORK),EINVAL);CHECK(!madvise(p,page,MADV_DONTFORK)&&!close(fd)&&!unlink(path));
    pid_t c=fork();CHECK(c>=0);if(!c){unsigned char v;ERROR(mincore(p,page,&v),ENOMEM);CHECK(p[page]==47);p[page]=91;_exit(0);}joined(c);
    CHECK(p[0]==35&&p[page]==47&&!munmap(p,2*page));pass(name);
}
static void fork_hole_remap(void)
{
    unsigned char *p=area(3,MAP_PRIVATE);p[0]=5;p[2*page]=9;CHECK(!munmap(p+page,page));
    ERROR(madvise(p,3*page,MADV_DONTFORK),ENOMEM);pid_t c=fork();CHECK(c>=0);
    if(!c){unsigned char v;ERROR(mincore(p,page,&v),ENOMEM);ERROR(mincore(p+2*page,page,&v),ENOMEM);_exit(0);}joined(c);
    CHECK(!munmap(p,3*page));p=area(1,MAP_PRIVATE);p[0]=18;CHECK(!madvise(p,page,MADV_WIPEONFORK));
    p=mremap(p,page,2*page,MREMAP_MAYMOVE);CHECK(p!=MAP_FAILED);c=fork();CHECK(c>=0);
    if(!c){CHECK(!p[0]&&!p[page]);_exit(0);}joined(c);CHECK(p[0]==18&&!munmap(p,2*page));pass("fork-policy-hole-prefix-and-remap");
}
static void fork_bss(void)
{
    bss[page]=93;CHECK(!madvise(bss+page,page,MADV_WIPEONFORK));pid_t c=fork();CHECK(c>=0);
    if(!c){CHECK(!bss[page]);_exit(0);}joined(c);CHECK(bss[page]==93&&!madvise(bss+page,page,MADV_KEEPONFORK));
    pass("wipe-pure-bss-zero-source");
}
struct fork_remote {unsigned char *p;unsigned cpu;atomic_uint ready,go;};
static void *fork_writer(void *opaque)
{
    struct fork_remote *w=opaque;cpu_set_t set;CPU_ZERO(&set);CPU_SET(w->cpu,&set);
    CHECK(!sched_setaffinity(0,sizeof(set),&set));CHECK(w->p[0]==31&&w->p[page]==63);
    atomic_store(&w->ready,1);while(!atomic_load(&w->go))sched_yield();
    w->p[0]=41;w->p[page]=71;return 0;
}
static void fork_remote(void)
{
    unsigned char *p=area(2,MAP_PRIVATE);p[0]=31;p[page]=63;CHECK(!madvise(p+page,page,MADV_WIPEONFORK));
    cpu_set_t set;CHECK(!sched_getaffinity(0,sizeof(set),&set));unsigned cpu=0;
    for(unsigned i=0;i<4;i++)if(CPU_ISSET(i,&set))cpu=i;
    struct fork_remote w={.p=p,.cpu=cpu};pthread_t thread;int done[2];CHECK(!pipe(done));
    CHECK(!pthread_create(&thread,0,fork_writer,&w));while(!atomic_load(&w.ready))sched_yield();
    pid_t c=fork();CHECK(c>=0);if(!c){expect(done[0]);CHECK(p[0]==31&&!p[page]);_exit(0);}
    atomic_store(&w.go,1);CHECK(!pthread_join(thread,0));token(done[1]);joined(c);
    CHECK(p[0]==41&&p[page]==71&&!close(done[0])&&!close(done[1])&&!munmap(p,2*page));
    pass("fork-remote-parent-cow-and-wipe");
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);page=(size_t)sysconf(_SC_PAGESIZE);CHECK(page==4096||page==16384);
    CHECK(!mkdir("/tmp",0755)||errno==EEXIST);CHECK(!mount("tmpfs","/tmp","tmpfs",0,0));
    abi();anonymous();cow();shared();file_case("/tmp/madvise","tmpfs-private-shared-data");
    file_case("/madvise","ext4-private-shared-data");locked();hole();willneed();remote();
    fork_absent();fork_wipe();fork_backing();fork_file("/tmp/fork-policy","dontfork-tmpfs-private-owner");
    fork_file("/fork-policy","dontfork-ext4-private-owner");fork_hole_remap();fork_bss();fork_remote();
    CHECK(!umount("/tmp"));printf("MADVISE END %u\n",cases);
    if(getpid()==1){sync();reboot(RB_POWER_OFF);}return 0;
}
