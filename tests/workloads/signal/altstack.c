#define _GNU_SOURCE
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <pthread.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "../../smp/composition_protocol.h"

#define AUTO (1U << 31)
#if defined(__loongarch__)
#define MINIMUM 4096U
#else
#define MINIMUM 2048U
#endif
static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("ALTSTACK FAIL line=%d %s errno=%d\n", __LINE__, #x, errno); __atomic_fetch_add(&failures,1,__ATOMIC_RELAXED); } } while (0)
static int error(long value, int code) { return value == -1 && errno == code; }
static int alt(const stack_t *input, stack_t *output) { return syscall(SYS_sigaltstack, input, output); }
static void abi(void)
{
    stack_t old, set, out;
    char *memory = mmap(0, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    CHECK(!alt(0, &old) && old.ss_flags == SS_DISABLE && !old.ss_sp && !old.ss_size);
    set = (stack_t){.ss_sp=memory, .ss_size=MINIMUM};
    CHECK(!alt(&set, &out) && out.ss_flags == SS_DISABLE);
    CHECK(!alt(0, &out) && out.ss_sp == memory && out.ss_size == MINIMUM && !out.ss_flags);
    set.ss_size--;
    CHECK(error(alt(&set, 0), ENOMEM));
    set.ss_size=MINIMUM; set.ss_flags=SS_ONSTACK;
    CHECK(!alt(&set, 0));
    CHECK(!alt(0, &out) && !out.ss_flags);
    for (unsigned variant=0; variant<2; variant++) for (unsigned flags=0; flags<16; flags++) {
        set.ss_flags=(int)(flags|(variant?AUTO:0));
        int valid=flags==0 || flags==SS_ONSTACK || flags==SS_DISABLE;
        CHECK(valid ? !alt(&set, 0) : error(alt(&set, 0), EINVAL));
    }
    set.ss_flags=SS_DISABLE;CHECK(!alt(&set,0));
    set.ss_flags=(int)AUTO;
    CHECK(!alt(&set, &set) && !set.ss_sp && set.ss_flags == SS_DISABLE);
    CHECK(!alt(0, &out) && (unsigned)out.ss_flags == AUTO);
    CHECK(error(alt((void *)UINTPTR_MAX, 0), EFAULT));
    CHECK(!alt(0, &out) && (unsigned)out.ss_flags == AUTO);
    set=(stack_t){.ss_sp=memory+16,.ss_size=32768};
    CHECK(error(alt(&set, (void *)UINTPTR_MAX), EFAULT));
    CHECK(!alt(0, &out) && out.ss_sp == set.ss_sp && out.ss_size == set.ss_size);
    /* Registration records an address; access is checked when delivering. */
    set=(stack_t){.ss_sp=(void *)UINTPTR_MAX,.ss_size=MINIMUM};
    CHECK(!alt(&set, 0));
    set.ss_flags=SS_DISABLE|(int)AUTO; set.ss_size=0;
    CHECK(!alt(&set, 0));
    CHECK(!alt(0, &out) && out.ss_flags == (int)(SS_DISABLE|AUTO) && !out.ss_sp && !out.ss_size);
    set.ss_flags=SS_DISABLE; CHECK(!alt(&set, 0));
    CHECK(!munmap(memory,65536));
}
static char *area, *replacement;
static volatile sig_atomic_t handler_bad, entered, nested, frame_mode;
static uintptr_t outer_address;
static int in_area(uintptr_t p, char *base) { return p>(uintptr_t)base && p<(uintptr_t)base+65536; }
#define HC(x) do { if (!(x)) handler_bad++; } while (0)
static void nested_handler(int sig, siginfo_t *info, void *context)
{
    char local; stack_t query; ucontext_t *uc=context;
    (void)sig; (void)info;
    HC(!alt(0,&query) && query.ss_flags==SS_ONSTACK);
    HC(in_area((uintptr_t)&local, frame_mode==5 ? replacement : area));
    if (frame_mode!=5) HC((uintptr_t)&local<outer_address);
    HC(!uc->uc_stack.ss_flags);
    nested++;
}
static void handler(int sig, siginfo_t *info, void *context)
{
    char local; stack_t query; ucontext_t *uc=context;
    (void)sig; (void)info;
    outer_address=(uintptr_t)&local;
    if(frame_mode>=6) {
        HC(!in_area(outer_address,area));
        HC(uc->uc_stack.ss_sp==area && uc->uc_stack.ss_size==65536);
        HC((unsigned)uc->uc_stack.ss_flags==(frame_mode==7?AUTO:0));
        HC(!alt(0,&query) && query.ss_flags==(frame_mode==7?SS_DISABLE:0));
        entered++;return;
    }
    HC(in_area(outer_address,area));
    HC(uc->uc_stack.ss_sp==area && uc->uc_stack.ss_size==65536);
    HC(!alt(0,&query));
    if (frame_mode==5) {
        HC(query.ss_flags==SS_DISABLE && (unsigned)uc->uc_stack.ss_flags==AUTO);
        stack_t next={.ss_sp=replacement,.ss_size=65536};
        HC(!alt(&next,0)); HC(!raise(SIGUSR2));
    } else {
        HC(query.ss_flags==SS_ONSTACK && !uc->uc_stack.ss_flags);
        stack_t invalid={.ss_flags=7};
        HC(error(alt(&invalid,0),EPERM));
        if (!frame_mode) HC(!raise(SIGUSR2));
        if (frame_mode==1) uc->uc_stack.ss_flags=7;
        if (frame_mode==2) uc->uc_stack.ss_size=1;
        if (frame_mode==3) uc->uc_stack=(stack_t){.ss_sp=replacement,.ss_size=65536};
        if (frame_mode==4) uc->uc_stack.ss_flags=SS_DISABLE;
    }
    entered++;
}
static void real_frames(void)
{
    area=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    replacement=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(area!=MAP_FAILED && replacement!=MAP_FAILED);
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_ONSTACK|SA_SIGINFO};
    sigemptyset(&action.sa_mask); CHECK(!sigaction(SIGUSR1,&action,0));
    action.sa_sigaction=nested_handler; CHECK(!sigaction(SIGUSR2,&action,0));
    for (frame_mode=0;frame_mode<=7;frame_mode++) {
        action.sa_sigaction=handler;action.sa_flags=SA_SIGINFO|(frame_mode>=6?0:SA_ONSTACK);
        CHECK(!sigaction(SIGUSR1,&action,0));
        stack_t s={.ss_sp=area,.ss_size=65536,.ss_flags=(frame_mode==5||frame_mode==7)?(int)AUTO:0},out;
        CHECK(!alt(&s,0)); CHECK(!raise(SIGUSR1)); CHECK(!handler_bad);
        CHECK(!alt(0,&out));
        if (frame_mode==4) CHECK(out.ss_flags==SS_DISABLE);
        else CHECK(out.ss_sp==(frame_mode==3?replacement:area) && out.ss_size==65536 &&
                   (unsigned)out.ss_flags==((frame_mode==5||frame_mode==7)?AUTO:0));
    }
    CHECK(entered==8 && nested==2);
    stack_t disabled={.ss_flags=SS_DISABLE}; CHECK(!alt(&disabled,0));
    CHECK(!munmap(area,65536) && !munmap(replacement,65536));
}
static void overflow_handler(int sig, siginfo_t *info, void *context)
{
    char local; (void)info; (void)context;
    _exit(sig==SIGSEGV && in_area((uintptr_t)&local,area) ? 0 : 91);
}
__attribute__((noinline)) static unsigned overflow(unsigned n)
{
    volatile char bytes[1024]; bytes[n%sizeof(bytes)]=(char)n;
    unsigned (*volatile next)(unsigned)=overflow;
    unsigned result=next(n+1);
    return result+(unsigned)bytes[n%sizeof(bytes)];
}
static void *overflow_thread(void *unused)
{
    (void)unused;
    stack_t s={.ss_sp=area,.ss_size=65536};
    if (alt(&s,0)) _exit(95);
    overflow(0); return 0;
}
static void exhausted(int sig)
{
    volatile char bytes[1024]; memset((void *)bytes,1,sizeof(bytes));
    (void)sig; raise(SIGUSR1); __asm__ volatile(""::"m"(bytes));
}
static char *relocated;
static void unreadable_stack(int sig, siginfo_t *info, void *context)
{
    (void)sig; (void)info;
    char *source=(char *)context-128;
    size_t bytes=1088;
#if defined(__loongarch__)
    bytes=576;
    for (;;) {
        uint32_t *record=(uint32_t *)(source+bytes);
        if (!record[0]) { bytes+=16; break; }
        bytes+=record[1];
    }
#endif
    char *sp=relocated+sysconf(_SC_PAGESIZE)-168;
    memcpy(sp,source,bytes);
    if (mprotect(relocated,(size_t)sysconf(_SC_PAGESIZE),PROT_NONE)) _exit(92);
#if defined(__riscv)
    __asm__ volatile("mv sp,%0; li a7,139; ecall; unimp"::"r"(sp):"memory");
#elif defined(__loongarch__)
    __asm__ volatile("move $sp,%0; li.d $a7,139; syscall 0; break 0"::"r"(sp):"memory");
#endif
    __builtin_unreachable();
}
static void bad_stacks(void)
{
    for (int mode=0;mode<6;mode++) {
        pid_t pid=fork();CHECK(pid>=0);
        if (!pid) {
            area=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            stack_t s={.ss_sp=area,.ss_size=65536}; CHECK(!alt(&s,0));
            struct sigaction action={.sa_sigaction=overflow_handler,.sa_flags=SA_ONSTACK|SA_SIGINFO};
            sigemptyset(&action.sa_mask);
            if (mode==0) {
                /* Alternate stacks are per thread; install in the child thread. */
                CHECK(!sigaction(SIGSEGV,&action,0));
                pthread_attr_t attr;pthread_t thread;
                CHECK(!pthread_attr_init(&attr));
                char *stack=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                CHECK(!mprotect(stack,4096,PROT_NONE));
                CHECK(!pthread_attr_setstack(&attr,stack+4096,65536-4096));
                CHECK(!pthread_create(&thread,&attr,overflow_thread,0));
                pthread_join(thread,0); _exit(93);
            }
            action.sa_sigaction=handler;CHECK(!sigaction(SIGUSR1,&action,0));
            if (mode==1) CHECK(!mprotect(area,65536,PROT_READ));
            if (mode==2) CHECK(!munmap(area,65536));
            if (mode==3) {
                action.sa_handler=exhausted;action.sa_flags=SA_ONSTACK|SA_NODEFER;
                CHECK(!sigaction(SIGUSR1,&action,0));
            }
            if (mode==4) {
                relocated=mmap(0,3*(size_t)sysconf(_SC_PAGESIZE),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                action.sa_sigaction=unreadable_stack; CHECK(!sigaction(SIGUSR1,&action,0));
            }
            if (mode==5) {
                s.ss_sp=(void *)(UINTPTR_MAX-15);CHECK(!alt(&s,0));
            }
            raise(SIGUSR1);_exit(94);
        }
        int status;CHECK(waitpid(pid,&status,0)==pid);
        CHECK(mode==0 ? WIFEXITED(status)&&WEXITSTATUS(status)==0 : WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
    }
}
static void *exec_thread(void *unused)
{
    (void)unused;
    stack_t s={.ss_sp=area,.ss_size=65536}; CHECK(!alt(&s,0));
    execl("/init","/init","--exec-disabled",(char *)0);_exit(96);
}
static void lifecycle(void)
{
    area=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    stack_t s={.ss_sp=area,.ss_size=65536},out;
    CHECK(!alt(&s,0));
    pid_t child=fork();CHECK(child>=0);
    if (!child) {
        if (alt(0,&out) || out.ss_sp!=area || out.ss_size!=65536) _exit(97);
        execl("/missing-executable","missing",(char *)0);
        if (errno!=ENOENT || alt(0,&out) || out.ss_sp!=area) _exit(98);
        execl("/init","/init","--exec-disabled",(char *)0);_exit(99);
    }
    int status;CHECK(waitpid(child,&status,0)==child && status==0);
    child=vfork();CHECK(child>=0);
    if (!child) {
        stack_t current;
        if (alt(0,&current) || current.ss_sp!=area) _exit(100);
        stack_t disabled={.ss_flags=SS_DISABLE};
        if (alt(&disabled,0)) _exit(101);
        _exit(0);
    }
    CHECK(waitpid(child,&status,0)==child && status==0);
    CHECK(!alt(0,&out) && out.ss_sp==area);
    child=fork();CHECK(child>=0);
    if (!child) {
        pthread_t thread;CHECK(!pthread_create(&thread,0,exec_thread,0));
        pthread_exit(0);
    }
    CHECK(waitpid(child,&status,0)==child && status==0);
    s.ss_flags=SS_DISABLE;CHECK(!alt(&s,0) && !munmap(area,65536));
}
static struct composition_control *control;
static int control_fd=-1;
static unsigned cpus;
static void pin(unsigned cpu)
{ uint64_t mask=UINT64_C(1)<<cpu;CHECK(!syscall(SYS_sched_setaffinity,0,sizeof(mask),&mask)); }
static void observe(void)
{
    if (access("/altstack-observer",F_OK)) return;
    CHECK(!mkdir("/dev/shm",0700)||errno==EEXIST);
    CHECK(!mount("tmpfs","/dev/shm","tmpfs",0,0)||errno==EBUSY);
    control_fd=open("/dev/shm/smp-control",O_CREAT|O_EXCL|O_RDWR,0600);
    CHECK(control_fd>=0 && !ftruncate(control_fd,4096));
    control=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,control_fd,0);
    CHECK(control!=MAP_FAILED);
    composition_store(&control->magic,COMPOSITION_MAGIC);
    while (!composition_load(&control->observer_ready)) sched_yield();
}
static void command(unsigned cmd)
{
    uint64_t n=composition_load(&control->request)+1;
    composition_store(&control->command,cmd);composition_store(&control->request,n);
    while (composition_load(&control->reply)!=n) sched_yield();
}
struct actor { unsigned cpu; int ready,go,done,bad; pid_t tid; char *stack; unsigned count; uintptr_t outer; };
static _Thread_local struct actor *current_actor;
static void remote_handler(int sig, siginfo_t *info, void *context)
{
    char local;struct actor *a=current_actor;stack_t out;ucontext_t *uc=context;
    (void)info;
    if (!a || !in_area((uintptr_t)&local,a->stack) || alt(0,&out) ||
        out.ss_flags!=SS_ONSTACK || uc->uc_stack.ss_sp!=a->stack || uc->uc_stack.ss_flags)
        { if(a)a->bad++;return; }
    if (sig==SIGUSR1) { a->outer=(uintptr_t)&local;raise(SIGUSR2); }
    else if ((uintptr_t)&local>=a->outer) a->bad++;
    a->count++;
}
static void *remote_thread(void *arg)
{
    struct actor *a=arg;current_actor=a;pin(a->cpu);
    stack_t out;
    if (alt(0,&out) || out.ss_flags!=SS_DISABLE) a->bad++;
    a->stack=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    stack_t s={.ss_sp=a->stack,.ss_size=65536};
    if (alt(&s,0)) a->bad++;
    a->tid=(pid_t)syscall(SYS_gettid);__atomic_store_n(&a->ready,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&a->go,__ATOMIC_ACQUIRE)) sched_yield();
    while (a->count<2) sched_yield();
    if (alt(0,&out) || out.ss_sp!=a->stack || out.ss_flags) a->bad++;
    s.ss_flags=SS_DISABLE;if(alt(&s,0)||munmap(a->stack,65536))a->bad++;
    __atomic_store_n(&a->done,1,__ATOMIC_RELEASE);return 0;
}
static void threads(void)
{
    uint64_t mask=0;CHECK(syscall(SYS_sched_getaffinity,0,sizeof(mask),&mask)>=0);
    cpus=(unsigned)__builtin_popcountll(mask);CHECK(cpus>=1 && cpus<=4);pin(0);
    struct sigaction action={.sa_sigaction=remote_handler,.sa_flags=SA_SIGINFO|SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGUSR1,&action,0) && !sigaction(SIGUSR2,&action,0));
    area=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    stack_t s={.ss_sp=area,.ss_size=65536},out;CHECK(!alt(&s,0));
    struct actor actors[4]={0};pthread_t tasks[4];
    for (unsigned i=0;i<cpus;i++) {actors[i].cpu=i;CHECK(!pthread_create(&tasks[i],0,remote_thread,&actors[i]));}
    for (unsigned i=0;i<cpus;i++) {
        while(!__atomic_load_n(&actors[i].ready,__ATOMIC_ACQUIRE))sched_yield();
        __atomic_store_n(&actors[i].go,1,__ATOMIC_RELEASE);
        CHECK(!syscall(SYS_tgkill,getpid(),actors[i].tid,SIGUSR1));
    }
    for (unsigned i=0;i<cpus;i++) {CHECK(!pthread_join(tasks[i],0));CHECK(actors[i].done && !actors[i].bad && actors[i].count==2);}
    CHECK(!alt(0,&out) && out.ss_sp==area);
    s.ss_flags=SS_DISABLE;CHECK(!alt(&s,0) && !munmap(area,65536));
}
static uint64_t race_go, race_ready, race_tid;
static int race_unmap;
static char *race_area;
static void *frame_racer(void *unused)
{
    (void)unused;pin(1);
    stack_t s={.ss_sp=race_area,.ss_size=65536};CHECK(!alt(&s,0));
    composition_store(&race_tid,(uint64_t)syscall(SYS_gettid));
    composition_store(&race_ready,1);
    while(!composition_load(&race_go))sched_yield();
    raise(SIGUSR1);_exit(102);
}
static void *mapping_racer(void *unused)
{
    (void)unused;pin(0);
    while(!composition_load(&control->read_entered))sched_yield();
    composition_store(&control->read_waiter_tid,(uint64_t)syscall(SYS_gettid));
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    int rc=race_unmap ? munmap(race_area+65536-page,page) :
                       mprotect(race_area+65536-page,page,PROT_NONE);
    if(rc)_exit(103);
    for(;;)sched_yield();
    return 0;
}
static void copy_races(void)
{
    if(!control || cpus<2) {puts("ALTSTACK CASE copy-races SKIP");return;}
    unsigned before=failures;
    for(unsigned mode=0;mode<2;mode++) {
        composition_store(&control->read_entered,0);composition_store(&control->read_release,0);
        composition_store(&control->read_waiter_tid,0);composition_store(&control->read_waited,0);
        int info[2],go[2];CHECK(!pipe(info) && !pipe(go));pid_t child=fork();CHECK(child>=0);
        if(!child) {
            close(info[0]);close(go[1]);race_unmap=(int)mode;
            race_area=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            /* Prepopulate so the hold is an actual write pin, not a fault setup. */
            memset(race_area,0,65536);race_go=race_ready=race_tid=0;
            pthread_t sender,mutator;
            CHECK(!pthread_create(&sender,0,frame_racer,0));
            while(!composition_load(&race_ready))sched_yield();
            uint64_t data[2]={race_tid,(uintptr_t)race_area};CHECK(write(info[1],data,sizeof(data))==sizeof(data));
            char byte;CHECK(read(go[0],&byte,1)==1);
            CHECK(!pthread_create(&mutator,0,mapping_racer,0));
            composition_store(&race_go,1);for(;;)sched_yield();
        }
        close(info[1]);close(go[0]);uint64_t data[2];CHECK(read(info[0],data,sizeof(data))==sizeof(data));
        composition_store(&control->read_tid,data[0]);composition_store(&control->read_address,data[1]);
        composition_store(&control->read_size,65536);composition_store(&control->read_hart,1);
        composition_store(&control->read_flags,COMPOSITION_READ_HOLD|COMPOSITION_READ_LOCAL|COMPOSITION_READ_SIGNAL);
        command(COMPOSITION_READ_ARM);CHECK(write(go[1],"x",1)==1);
        int status;CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
        CHECK(composition_load(&control->read_entered)==1 && composition_load(&control->read_waited)==(mode?2U:1U));
        command(COMPOSITION_READ_FINISH);CHECK(!close(info[0]) && !close(go[1]));
    }
    printf("ALTSTACK CASE copy-races %s\n",before==failures?"OK":"FAIL");
}
#if defined(__loongarch__)
/* LP64S allocates no FPRs, but these instructions deliberately trigger the
 * hardware extension state. LP64D also declares its caller-saved clobber. */
#if __loongarch_frlen
#define WIDTH_CLOBBERS "$f0", "memory"
#else
#define WIDTH_CLOBBERS "memory"
#endif
static unsigned frame_width, width_bytes;
static void width_handler(int signal,siginfo_t *info,void *context)
{
    char local;(void)info;char *frame=(char *)context-128;
    uint32_t *extension=(uint32_t *)(frame+576);
    unsigned magic=frame_width==4?0x41535801U:frame_width==2?0x53580001U:0x46505501U;
    if(signal!=SIGUSR1 || extension[0]!=magic || extension[1]<16+256*frame_width+12 ||
       !in_area((uintptr_t)&local,area))_exit(105);
    width_bytes=576+extension[1]+16;
    if(frame<area+65536-4096 || frame+width_bytes>area+65536)_exit(106);
}
static void width_frames(void)
{
    uint32_t config,index=2;__asm__ volatile("cpucfg %0,%1":"=r"(config):"r"(index));
    for(unsigned width=1;width<=4;width*=2) {
        if(width>1 && !(config&(width==2?64U:128U)))continue;
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            area=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(mprotect(area+65536-8192,4096,PROT_NONE))_exit(107);
            stack_t s={.ss_sp=area+65536-4096,.ss_size=4096};if(alt(&s,0))_exit(108);
            struct sigaction action={.sa_sigaction=width_handler,.sa_flags=SA_SIGINFO|SA_ONSTACK};
            sigemptyset(&action.sa_mask);if(sigaction(SIGUSR1,&action,0))_exit(109);
            frame_width=width;
            if(width==4)__asm__ volatile("xvxor.v $xr0,$xr0,$xr0":::WIDTH_CLOBBERS);
            else if(width==2)__asm__ volatile("vxor.v $vr0,$vr0,$vr0":::WIDTH_CLOBBERS);
            else __asm__ volatile("movgr2fr.d $f0,$zero":::WIDTH_CLOBBERS);
            if(raise(SIGUSR1) || !width_bytes)_exit(110);
            /* Leave 16 bytes less than this actual width's frame. The guard
             * below the registered stack also verifies Linux's copy failure. */
            uintptr_t sp=(uintptr_t)s.ss_sp+width_bytes-16;
            long pid=getpid(),tid=syscall(SYS_gettid);
            __asm__ volatile("move $sp,%0; move $a0,%1; move $a1,%2; li.d $a2,10; li.d $a7,131; syscall 0; li.d $a0,111; li.d $a7,94; syscall 0; break 0"
                ::"r"(sp),"r"(pid),"r"(tid):"$a0","$a1","$a2","$a7","memory");
            __builtin_unreachable();
        }
        int status;CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    }
}
#else
static void width_frames(void) { }
#endif
static void run(const char *name, void (*test)(void))
{ unsigned before=failures;test();printf("ALTSTACK CASE %s %s\n",name,before==failures?"OK":"FAIL"); }
int main(int argc, char **argv)
{
    setvbuf(stdout,0,_IONBF,0); alarm(15);
    if (argc==2 && !strcmp(argv[1],"--exec-disabled")) {
        stack_t s;return alt(0,&s) || s.ss_flags!=SS_DISABLE || s.ss_sp || s.ss_size;
    }
    run("abi",abi);run("frames",real_frames);run("bad-stacks",bad_stacks);run("lifecycle",lifecycle);
    width_frames();
    observe();run("threads",threads);
    copy_races();
    if(control) {
        command(COMPOSITION_STOP);CHECK(!munmap(control,4096) && !close(control_fd));
        CHECK(!unlink("/dev/shm/smp-control"));control=0;
    }
    printf("ALTSTACK END %u\n",failures?0:6);
    return failures ? 1 : 0;
}
