#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <elf.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MS UINT64_C(1000000)
static volatile struct shared {
    unsigned stage, peer;
    uint64_t start, resumed, peer_at, count, phase;
} *s;
static void require(int ok, const char *what)
{ if (!ok) { dprintf(2,"sched bandwidth FAIL %s errno=%d\n",what,errno); exit(1); } }
static uint64_t now(void)
{ struct timespec t; require(clock_gettime(CLOCK_MONOTONIC,&t)==0,"clock"); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void spin(uint64_t duration)
{ uint64_t start=now(); while(now()-start<duration) { } }
static void policy(pid_t pid,int kind,int priority)
{ struct sched_param p={.sched_priority=priority}; require(syscall(SYS_sched_setscheduler,pid,kind,&p)==0,"setscheduler"); }
static void control(const char *field,long value)
{
    char path[96],text[32]; snprintf(path,sizeof(path),"/proc/sys/kernel/sched_rt_%s_us",field);
    int fd=open(path,O_WRONLY); require(fd>=0,"control open");
    int n=snprintf(text,sizeof(text),"%ld\n",value); require(write(fd,text,n)==n,"control write"); require(close(fd)==0,"control close");
}
static void configure(long period,long runtime)
{ control("runtime",-1); control("period",period); control("runtime",runtime); }
static void reap(pid_t child)
{ int status; require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"child status"); }
static void fresh_period(uint64_t ns)
{ struct timespec delay={.tv_sec=ns/1000000000,.tv_nsec=ns%1000000000}; require(nanosleep(&delay,0)==0,"fresh period"); }

static void consumed_configuration(void)
{
    int gate[2]; require(pipe(gate)==0,"pipe");
    configure(500000,20000); fresh_period(510*MS); memset((void *)s,0,sizeof(*s));
    pid_t child=fork(); require(child>=0,"fork");
    if(!child) {
        s->start=now(); s->stage=1;
        uint64_t limit=s->start+2000*MS;
        while(s->stage!=2 && now()<limit) s->count++;
        if(s->stage!=2) _exit(2);
        spin(10*MS); s->stage=3;
        char c; if(read(gate[0],&c,1)!=1) _exit(3);
        s->resumed=now(); s->stage=4; _exit(0);
    }
    policy(child,SCHED_FIFO,60);
    require(s->stage==1,"FIFO quota gives OTHER CPU");
    uint64_t first=now(), count=s->count;
    control("runtime",20000); control("period",500000);
    spin(20*MS);
    require(s->stage==1 && s->count==count,"same-value writes preserve exhaustion");
    s->stage=2; control("runtime",-1);
    require(s->stage==3,"disabled quota resumes RT");
    control("runtime",20000);
    require(write(gate[1],"x",1)==1,"wake exhausted RT");
    spin(20*MS); require(s->stage==3,"reenable preserves consumed runtime");
    uint64_t deadline=now()+1000*MS;
    while(s->stage!=4 && now()<deadline) { }
    require(s->stage==4 && s->resumed>first+100*MS,"next period resumes RT");
    printf("BoarOS: sched budget configuration ok first_ns=%llu resume_ns=%llu\n",(unsigned long long)(first-s->start),(unsigned long long)(s->resumed-s->start));
    reap(child); close(gate[0]); close(gate[1]);
}
static void short_period(void)
{
    configure(500,250); fresh_period(2*MS); memset((void *)s,0,sizeof(*s));
    pid_t child=fork(); require(child>=0,"short fork");
    if(!child) {
        struct timespec coarse;
        require(clock_gettime(CLOCK_MONOTONIC_COARSE,&coarse)==0,"coarse phase");
        uint64_t tick=(uint64_t)coarse.tv_sec*1000000000+coarse.tv_nsec, next=tick;
        do {
            require(clock_gettime(CLOCK_MONOTONIC_COARSE,&coarse)==0,"coarse edge");
            next=(uint64_t)coarse.tv_sec*1000000000+coarse.tv_nsec;
        } while(next==tick);
        s->start=now(); s->phase=s->start-next; s->stage=1;
        uint64_t deadline=s->start+50*MS;
        while(s->stage==1 && now()<deadline) s->count++;
        _exit(s->stage==2?0:4);
    }
    policy(child,SCHED_FIFO,60);
    uint64_t started_deadline=now()+50*MS;
    while(!s->stage && now()<started_deadline) { }
    require(s->stage==1,"short quota preempts busy FIFO");
    uint64_t elapsed=now()-s->start;
    s->stage=2; reap(child);
    /* A 10 ms periodic tick cannot satisfy this bound. Log actual overrun. */
    require(s->phase<MS,"short budget starts near new coarse tick");
    require(elapsed<8*MS,"sub-tick budget deadline");
    printf("BoarOS: sched short budget ok elapsed_ns=%llu phase_ns=%llu overrun_ns=%llu\n",(unsigned long long)elapsed,(unsigned long long)s->phase,(unsigned long long)(elapsed>250000?elapsed-250000:0));
}
static void rr_preemption(void)
{
    configure(1000000,-1); memset((void *)s,0,sizeof(*s));
    int a[2],b[2],h[2]; require(pipe(a)==0 && pipe(b)==0 && pipe(h)==0,"RR pipes");
    pid_t high=fork(); require(high>=0,"high fork");
    if(!high) { char c; if(read(h[0],&c,1)!=1) _exit(5); spin(60*MS); _exit(0); }
    pid_t peer=fork(); require(peer>=0,"peer fork");
    if(!peer) { char c; if(read(b[0],&c,1)!=1) _exit(6); s->peer_at=now(); s->peer=1; _exit(0); }
    pid_t first=fork(); require(first>=0,"first fork");
    if(!first) {
        char c; if(read(a[0],&c,1)!=1) _exit(7);
        s->start=now(); spin(60*MS);
        if(write(b[1],"b",1)!=1 || write(h[1],"h",1)!=1) _exit(8);
        s->resumed=now(); uint64_t deadline=s->resumed+300*MS;
        while(!s->peer && now()<deadline) { }
        _exit(s->peer?0:9);
    }
    policy(high,SCHED_FIFO,60); policy(peer,SCHED_RR,40); policy(first,SCHED_RR,40);
    require(write(a[1],"a",1)==1,"start RR");
    reap(first); reap(peer); reap(high);
    require(s->peer_at>=s->resumed,"peer runs after high priority returns");
    uint64_t remaining=s->peer_at-s->resumed;
    require(remaining<80*MS,"RR preemption retains remaining slice");
    printf("BoarOS: sched RR preemption ok remaining_ns=%llu\n",(unsigned long long)remaining);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]); close(h[0]); close(h[1]);
}
static void read_pid_stat(pid_t pid,unsigned long long fields[42])
{
    char path[64]; snprintf(path,sizeof(path),"/proc/%ld/stat",(long)pid);
    int fd=open(path,O_RDONLY); require(fd>=0,"stat open");
    char text[1024]; ssize_t n=read(fd,text,sizeof(text)-1); require(n>0,"stat read"); close(fd); text[n]=0;
    char *cursor=strrchr(text,')'); require(cursor && cursor[1]==' ',"stat comm");
    fields[3]=(unsigned char)cursor[2];
    cursor+=4; unsigned field=4;
    while(field<=41 && *cursor) {
        char *end; fields[field]=strtoull(cursor,&end,10); if(end==cursor) break;
        cursor=end; if(*cursor==' ') cursor++; field++;
    }
    require(field==42,"stat has scheduler fields");
}
static void read_stat(unsigned long long fields[42])
{ read_pid_stat(getpid(),fields); }
static void proc_policy(void)
{
    unsigned long long fields[42]={0},before[42]={0};
    read_stat(fields);
    require((long long)fields[18]==20 && fields[40]==0 && fields[41]==0,"stat OTHER");
    require(fields[26] <= (uintptr_t)proc_policy && fields[27] > (uintptr_t)proc_policy && fields[28] > (uintptr_t)&fields,"stat executable and initial stack");
    int elf=open("/proc/self/exe",O_RDONLY); require(elf>=0,"elf open");
    Elf64_Ehdr eh; require(read(elf,&eh,sizeof(eh))==sizeof(eh),"elf header");
    unsigned long long code_start=UINT64_MAX,code_end=0;
    for(unsigned i=0;i<eh.e_phnum;i++) {
        Elf64_Phdr ph; require(pread(elf,&ph,sizeof(ph),eh.e_phoff+i*eh.e_phentsize)==sizeof(ph),"elf phdr");
        if(ph.p_type!=PT_LOAD || !(ph.p_flags&PF_X)) continue;
        if(ph.p_vaddr<code_start) code_start=ph.p_vaddr;
        if(ph.p_vaddr+ph.p_filesz>code_end) code_end=ph.p_vaddr+ph.p_filesz;
    }
    close(elf); require(fields[26]==code_start && fields[27]==code_end,"stat exact main ELF file extent");
    require(fields[35]==0,"running wchan");
    pid_t stopped=fork(); require(stopped>=0,"stopped fork");
    if(!stopped) { raise(SIGSTOP); _exit(0); }
    int stop_status;
    require(waitpid(stopped,&stop_status,WUNTRACED)==stopped && WIFSTOPPED(stop_status),"stopped wait");
    read_pid_stat(stopped,fields); require(fields[35]==1,"stopped wchan");
    require(kill(stopped,SIGCONT)==0,"continue"); reap(stopped);
    policy(0,SCHED_FIFO,23); read_stat(fields);
    require((long long)fields[18]==-24 && fields[40]==23 && fields[41]==SCHED_FIFO,"stat FIFO");
    policy(0,SCHED_RR,35); read_stat(fields);
    require((long long)fields[18]==-36 && fields[40]==35 && fields[41]==SCHED_RR,"stat RR");
    memcpy(before,fields,sizeof(fields));
    pid_t child=fork(); require(child>=0,"stat fork");
    if(!child) {
        read_stat(fields);
        require(fields[26]==before[26] && fields[27]==before[27] && fields[28]==before[28],"stat fork metadata");
        char *args[]={"missing",0}; execv("/no-such-executable",args);
        require(errno==ENOENT,"failed exec"); read_stat(fields);
        require(fields[26]==before[26] && fields[27]==before[27] && fields[28]==before[28],"failed exec keeps metadata");
        _exit(0);
    }
    reap(child); policy(0,SCHED_OTHER,0);
    puts("BoarOS: sched proc stat ok");
}
static void rr_quota_boundary(void)
{
    configure(300000,-1); memset((void *)s,0,sizeof(*s));
    int gate[2]; require(pipe(gate)==0,"boundary gate");
    pid_t children[2];
    for(unsigned id=0;id<2;id++) {
        children[id]=fork(); require(children[id]>=0,"boundary fork");
        if(!children[id]) {
            char c; if(read(gate[0],&c,1)!=1) _exit(10);
            if(!s->peer) s->peer=id+1;
            else s->count++;
            uint64_t deadline=now()+2000*MS;
            while(!s->stage && now()<deadline) { }
            _exit(s->stage?0:11);
        }
    }
    /* Both block as OTHER before assigning pristine 100 ms RR slices. */
    for(unsigned id=0;id<2;id++) {
        unsigned long long fields[42]; uint64_t deadline=now()+1000*MS;
        do { read_pid_stat(children[id],fields); if(fields[3]=='S') break; spin(MS); } while(now()<deadline);
        require(fields[3]=='S',"RR gate blocked"); policy(children[id],SCHED_RR,40);
    }
    control("runtime",100000); fresh_period(310*MS);
    require(write(gate[1],"ab",2)==2,"release RR pair");
    require(s->peer!=0,"first RR ran");
    uint64_t first_stop=now(),deadline=first_stop+450*MS;
    while(!s->count && now()<deadline) { }
    int rotated=s->count!=0;
    s->stage=1; control("runtime",-1);
    reap(children[0]); reap(children[1]); close(gate[0]); close(gate[1]);
    require(rotated,"RR simultaneous quota and slice expiry rotates peer");
    printf("BoarOS: sched RR quota boundary ok observed_ns=%llu\n",(unsigned long long)(now()-first_stop));
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    require(mkdir("/proc",0755)==0 || errno==EEXIST,"proc dir");
    require(mount("proc","/proc","proc",0,0)==0,"proc mount");
    s=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    require(s!=MAP_FAILED,"shared map");
    proc_policy(); consumed_configuration(); short_period(); rr_preemption(); rr_quota_boundary();
    configure(1000000,950000);
    require(munmap((void *)s,4096)==0,"unmap");
    puts("BoarOS: sched bandwidth checks ok"); return 42;
}
