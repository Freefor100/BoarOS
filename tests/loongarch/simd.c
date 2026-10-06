#define _GNU_SOURCE
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fenv.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"LA SIMD line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
struct info {uint32_t magic,size;uint64_t padding;};
struct arrays {uint64_t input[32][4],output[32][4];} __attribute__((aligned(32)));
int la_simd_probe(const uint64_t input[32][4],uint64_t output[32][4],unsigned lanes);
long la_simd_fork(const uint64_t input[32][4],uint64_t output[32][4],unsigned lanes);
void la_simd_initial(uint64_t low,uint64_t output[4],unsigned lanes);
static volatile sig_atomic_t frame_mode,nested,pending_seen;
static void nested_handler(int signal)
{ if(signal!=SIGUSR1)_exit(84);nested++;__asm__ volatile("vxor.v $vr0,$vr0,$vr0":::"$f0"); }
static void handler(int sig,siginfo_t *info,void *pointer)
{
    ucontext_t *u=pointer;
    if(sig==SIGFPE){if(info->si_code!=SI_KERNEL || info->si_addr)_exit(88);pending_seen++;return;}
    if(sig!=SIGTRAP)_exit(80);
    unsigned lanes=(unsigned)u->uc_mcontext.__gregs[6];
    const uint64_t (*input)[4]=(void *)(uintptr_t)u->uc_mcontext.__gregs[4];
    struct info header;memcpy(&header,u->uc_mcontext.__extcontext,16);
    unsigned bytes=32*lanes*8;
    if(!(u->uc_mcontext.__flags&1) || header.magic!=(lanes==4 ? 0x41535801U : 0x53580001U) || header.size<16+bytes+16)_exit(81);
    unsigned char *payload=(void *)((char *)u->uc_mcontext.__extcontext+16);
    for(unsigned i=0;i<32;i++)for(unsigned j=0;j<lanes;j++) {
        uint64_t value;memcpy(&value,payload+(i*lanes+j)*8,8);
        if(value!=input[i][j])_exit(82);
    }
    uint64_t fcc;uint32_t fcsr;memcpy(&fcc,payload+bytes,8);memcpy(&fcsr,payload+bytes+8,4);
    if(fcc!=UINT64_C(0x0100010001000100) || (fcsr&0x300)!=0x100)_exit(83);
    uint64_t changed=input[17][lanes-1]^UINT64_C(0xabcdeffedcba1234);
    memcpy(payload+(17*lanes+lanes-1)*8,&changed,8);
    u->uc_mcontext.__pc+=4;
    if(frame_mode==1){header.magic=0xbadbeef;memcpy(u->uc_mcontext.__extcontext,&header,16);return;}
    if(frame_mode==2){header.size=16+bytes+15;memcpy(u->uc_mcontext.__extcontext,&header,16);return;}
    if(frame_mode==3) {
        unsigned semantic=bytes+12;
        char *pages=mmap(0,49152,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(pages==MAP_FAILED)_exit(85);
        char *base=pages+16384-592-semantic;memcpy(base,(char *)u-128,592+semantic);
        header.size=16384+16+semantic;memcpy(base+576,&header,16);memset(pages+32768,0,16);
        if(mprotect(pages+16384,16384,PROT_NONE))_exit(86);
        __asm__ volatile("move $sp,%0;li.w $a7,139;syscall 0"::"r"(base):"memory");__builtin_unreachable();
    }
    if(frame_mode==4){if(raise(SIGUSR1) || nested!=1)_exit(87);}
    if(frame_mode==5)u->uc_mcontext.__flags&=~1U;
    if(frame_mode==6 || frame_mode==7 || frame_mode==8) {
        char *pages=mmap(0,32768,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(pages==MAP_FAILED)_exit(85);
        char *base=pages+8;memcpy(base,(char *)u-128,576);
        unsigned size=16+((bytes+19)&~7U),offset=576;
        if(frame_mode==6) {
            header.size=size;memcpy(base+offset,&header,16);memset(base+offset+16,0,size-16);offset+=size;
        }
        header.size=size;memcpy(base+offset,&header,16);memcpy(base+offset+16,payload,bytes+12);
        if(frame_mode>=7){uint32_t csr=UINT32_C(0x01000001);memcpy(base+offset+16+bytes+8,&csr,4);}
        offset+=size;
        if(frame_mode==6){struct info scalar={0x46505501,288,0};memcpy(base+offset,&scalar,16);memset(base+offset+16,0,272);offset+=288;}
        memset(base+offset,0,16);
        if(frame_mode==8 && mprotect(pages,16384,PROT_READ))_exit(86);
        __asm__ volatile("move $sp,%0;li.w $a7,139;syscall 0"::"r"(base):"memory");__builtin_unreachable();
    }
    if(lanes==4)__asm__ volatile("xvxor.v $xr0,$xr0,$xr0":::"$f0");
    else __asm__ volatile("vxor.v $vr0,$vr0,$vr0":::"$f0");
}
static int run(unsigned lanes,unsigned id)
{
    struct arrays state;
    for(unsigned i=0;i<32;i++)for(unsigned j=0;j<4;j++)state.input[i][j]=UINT64_C(0x1234567812340000)+(id<<12)+(i<<4)+j;
    CHECK(!la_simd_probe(state.input,state.output,lanes));
    for(unsigned i=0;i<32;i++)for(unsigned j=0;j<lanes;j++) {
        uint64_t expected=state.input[i][j];if(i==17 && j==lanes-1)expected^=UINT64_C(0xabcdeffedcba1234);
        CHECK(state.output[i][j]==expected);
    }
    return 0;
}
static void *worker(void *p)
{ unsigned value=(unsigned)(uintptr_t)p;return (void *)(uintptr_t)run(value&7,value>>3); }
int main(int argc,char **argv)
{
    uint32_t config,index=2;__asm__ volatile("cpucfg %0,%1":"=r"(config):"r"(index));
    if(argc==2 && !strcmp(argv[1],"execed")) {
        uint64_t out[4] __attribute__((aligned(32)));
        la_simd_initial(UINT64_MAX,out,(config&128) ? 4 : 2);
        for(unsigned i=0;i<((config&128) ? 4U : 2U);i++)CHECK(out[i]==UINT64_MAX);
        CHECK(fegetround()==FE_TONEAREST);
        return 0;
    }
    struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO};CHECK(!sigaction(SIGTRAP,&action,0));
    CHECK(!sigaction(SIGFPE,&action,0));
    struct sigaction nested_action={.sa_handler=nested_handler};CHECK(!sigaction(SIGUSR1,&nested_action,0));
    for(unsigned lanes=2;lanes<=4;lanes*=2) {
        int supported=(config&(lanes==2 ? 64U : 128U))!=0;
        pid_t child=fork();CHECK(child>=0);
        if(!child){uint64_t out[4] __attribute__((aligned(32)));la_simd_initial(42,out,lanes);if(out[0]!=42)_exit(1);for(unsigned j=1;j<lanes;j++)if(out[j]!=UINT64_MAX)_exit(2);_exit(run(lanes,1));}
        int status;CHECK(waitpid(child,&status,0)==child);
        if(supported)CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
        else CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGILL);
        CHECK(((getauxval(AT_HWCAP)&(lanes==2 ? 16U : 32U))!=0)==supported);
        if(!supported)continue;
        pthread_t a,b;CHECK(!pthread_create(&a,0,worker,(void *)(uintptr_t)(lanes|(2<<3))) &&
            !pthread_create(&b,0,worker,(void *)(uintptr_t)(lanes|(3<<3))));
        void *result;CHECK(!pthread_join(a,&result) && !result && !pthread_join(b,&result) && !result);
        struct arrays inherited;
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<4;j++)inherited.input[i][j]=UINT64_C(0xcdef000000000000)+(i<<4)+j;
        child=(pid_t)la_simd_fork(inherited.input,inherited.output,lanes);CHECK(child>=0);
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<lanes;j++) {
            uint64_t expected=!child && j ? UINT64_MAX : inherited.input[i][j];
            if(inherited.output[i][j]!=expected) {
            fprintf(stderr,"SIMD fork child=%d width=%u reg=%u lane=%u expected=%llx observed=%llx\n",child,lanes,i,j,
                (unsigned long long)expected,(unsigned long long)inherited.output[i][j]);return 1;
            }
        }
        if(!child)_exit(0);
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        for(unsigned mode=1;mode<=8;mode++) {
            child=fork();CHECK(child>=0);
            if(!child){frame_mode=mode;int result=run(lanes,4);_exit(result || (mode==7 && pending_seen!=1));}
            CHECK(waitpid(child,&status,0)==child);
            if(mode<3 || mode==8)CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
            else CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
        }
        child=fork();CHECK(child>=0);
        if(!child){if(run(lanes,8) || fesetround(FE_DOWNWARD))_exit(3);execl("/init","/init","execed",(char *)0);_exit(3);}
        CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("LA SIMD first-use/all lanes/timer/signal/fork/exec/HWCAP passed");return 0;
}
