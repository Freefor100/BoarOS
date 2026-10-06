#define _GNU_SOURCE
#include <fenv.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"LA FPU line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
struct info {uint32_t magic,size;uint64_t padding;};
struct fp {uint64_t regs[32],fcc;uint32_t fcsr,padding;};
static volatile sig_atomic_t seen,fpe,frame_mode;
static volatile sig_atomic_t fpe_code;
static volatile uint32_t fpe_csr;
int la_fp_register_probe(uint64_t pattern);
static void handler(int sig,siginfo_t *info,void *pointer)
{
    (void)info;ucontext_t *u=pointer;struct info header;struct fp state;
    memcpy(&header,u->uc_mcontext.__extcontext,sizeof(header));
    if((sig!=SIGUSR1 && sig!=SIGTRAP && sig!=SIGFPE) || !(u->uc_mcontext.__flags&1) || header.magic!=0x46505501 || header.size!=288)_exit(80);
    memcpy(&state,(char *)u->uc_mcontext.__extcontext+16,sizeof(state));
    if(sig==SIGUSR1 && frame_mode) {
        if(frame_mode==1){header.magic=0xdeadabcd;memcpy(u->uc_mcontext.__extcontext,&header,sizeof(header));return;}
        if(frame_mode==2){header.size=287;memcpy(u->uc_mcontext.__extcontext,&header,sizeof(header));return;}
        if(frame_mode==3) {
            char *pages=mmap(0,32768,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(pages==MAP_FAILED)_exit(87);
            char *base=pages+16384-872;memcpy(base,(char *)u-128,880);
            if(mprotect(pages+16384,16384,PROT_NONE))_exit(88);
            __asm__ volatile("move $sp,%0;li.w $a7,139;syscall 0"::"r"(base):"memory");__builtin_unreachable();
        }
        if(frame_mode==5) {
            char *pages=mmap(0,49152,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(pages==MAP_FAILED)_exit(87);
            /* 有效 FP 字段止于页尾；记录跳过不可读 padding 后仍有可读 END。 */
            char *base=pages+16384-860;memcpy(base,(char *)u-128,860);
            struct info padded={0x46505501,16384+284,0};memcpy(base+576,&padded,16);memset(pages+32768,0,16);
            if(mprotect(pages+16384,16384,PROT_NONE))_exit(88);
            __asm__ volatile("move $sp,%0;li.w $a7,139;syscall 0"::"r"(base):"memory");__builtin_unreachable();
        }
        state.fcsr|=UINT32_C(0x10000010);memcpy((char *)u->uc_mcontext.__extcontext+16,&state,sizeof(state));return;
    }
    if(sig==SIGFPE) {
        if(frame_mode==4) {if(info->si_code!=SI_KERNEL || info->si_addr)_exit(89);}
        else if(info->si_addr!=(void *)u->uc_mcontext.__pc)_exit(86);
        fpe_code=info->si_code;fpe_csr=state.fcsr;
        state.fcsr&=~31U;memcpy((char *)u->uc_mcontext.__extcontext+16,&state,sizeof(state));
        if(frame_mode!=4)u->uc_mcontext.__pc+=4;
        fpe++;return;
    }
    if(sig==SIGTRAP) {
        uint64_t base=u->uc_mcontext.__gregs[4];
        for(unsigned i=0;i<32;i++)if(state.regs[i]!=base+i)_exit(84);
        if(state.fcc!=UINT64_C(0x0100010001000100) || (state.fcsr&0x300)!=0x100)_exit(85);
        __asm__ volatile("movgr2fr.d $f0,$zero;movgr2cf $fcc3,$zero");
        u->uc_mcontext.__pc+=4;return;
    }
    if((state.fcsr&0x300)!=0x300 || state.regs[24]!=UINT64_C(0x400921fb54442d18))_exit(81);
    /* Handler changes both the hardware and saved user context. */
    if(fesetround(FE_UPWARD))_exit(82);
    state.fcsr=(state.fcsr&~0x300U)|0x100U;state.regs[24]=UINT64_C(0x4005bf0a8b145769);
    memcpy((char *)u->uc_mcontext.__extcontext+16,&state,sizeof(state));seen++;
}
static int inherited(void)
{
    pid_t child=fork();CHECK(child>=0);
    if(!child)_exit(fegetround()==FE_DOWNWARD ? 0 : 2);
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));return 0;
}
static void *worker(void *pointer)
{ return (void *)(uintptr_t)la_fp_register_probe(UINT64_C(0x1234000000000000)+(uintptr_t)pointer*256); }
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"execed")) {
        uint64_t first;__asm__ volatile("movfr2gr.d %0,$f0":"=r"(first));
        CHECK(first==UINT64_MAX && fegetround()==FE_TONEAREST);return 0;
    }
    CHECK(!fesetround(FE_DOWNWARD) && fegetround()==FE_DOWNWARD && !inherited());
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};CHECK(!sigaction(SIGUSR1,&action,0));
    uint64_t original=UINT64_C(0x400921fb54442d18),observed;
    __asm__ volatile("movgr2fr.d $f24,%0"::"r"(original):"$f24");
    CHECK(!raise(SIGUSR1) && seen==1 && fegetround()==FE_TOWARDZERO);
    __asm__ volatile("movfr2gr.d %0,$f24":"=r"(observed));CHECK(observed==UINT64_C(0x4005bf0a8b145769));
    CHECK(!fesetround(FE_TONEAREST));
    volatile double a=1.25,b=2.5;double answer=a*b+0.125;CHECK(answer==3.25);
    CHECK(!feclearexcept(FE_ALL_EXCEPT) && !feraiseexcept(FE_DIVBYZERO) && (fetestexcept(FE_DIVBYZERO)&FE_DIVBYZERO));
    CHECK(!sigaction(SIGFPE,&action,0));
    uint64_t one=UINT64_C(0x3ff0000000000000);uint32_t enable=8;
    __asm__ volatile("movgr2fr.d $f1,%0;movgr2fr.d $f2,$zero;movgr2fcsr $fcsr0,%1;fdiv.d $f0,$f1,$f2"::"r"(one),"r"(enable):"$f0","$f1","$f2");
    CHECK(fpe==1 && fpe_code==FPE_FLTDIV);
    const uint64_t operands[]={UINT64_C(0x7fefffffffffffff),UINT64_C(0x0010000000000000)};
    const int codes[]={FPE_FLTOVF,FPE_FLTUND};
    const uint32_t remaining[]={UINT32_C(0x04000001),UINT32_C(0x02000001)};
    for(unsigned i=0;i<2;i++) {
        /* 只启用 inexact；分类仍需包含同时产生的 overflow/underflow Cause。 */
        enable=1;
        __asm__ volatile("movgr2fr.d $f1,%0;movgr2fcsr $fcsr0,%1;fmul.d $f0,$f1,$f1"::"r"(operands[i]),"r"(enable):"$f0","$f1");
        CHECK(fpe==(sig_atomic_t)i+2 && fpe_code==codes[i] && fpe_csr==remaining[i]);
    }
    puts("LA FPU exception signal passed");
    CHECK(!sigaction(SIGTRAP,&action,0));
    pthread_t threads[2];for(uintptr_t i=0;i<2;i++)CHECK(!pthread_create(&threads[i],0,worker,(void *)i));
    for(unsigned i=0;i<2;i++){void *result;CHECK(!pthread_join(threads[i],&result) && !result);}
    CHECK(!fesetround(FE_DOWNWARD));pid_t child=fork();CHECK(child>=0);
    if(!child){execl("/init","/init","execed",(char *)0);_exit(2);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    for(frame_mode=1;frame_mode<=5;frame_mode++) {
        child=fork();CHECK(child>=0);
        if(!child){fpe=0;if(raise(SIGUSR1))_exit(1);_exit(frame_mode==4 && fpe!=1 ? 2 : 0);}
        CHECK(waitpid(child,&status,0)==child);
        if(frame_mode<3)CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
        else CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("LA FPU extension/badframe/pending/cross-page END passed");
    puts("LA FPU registers/FCC/timer/exec passed");
    puts("LA FPU arithmetic/fenv/fork/signal passed");return 0;
}
