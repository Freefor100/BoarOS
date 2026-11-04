#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__riscv)
#ifndef SYS_riscv_flush_icache
#define SYS_riscv_flush_icache 259
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"ICACHE FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static int cpu_ids[CPU_SETSIZE],cpu_count;
static void pin(int cpu)
{
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    CHECK(!sched_setaffinity(0,sizeof(set),&set));
    cpu_set_t actual;CPU_ZERO(&actual);CHECK(!sched_getaffinity(0,sizeof(actual),&actual) && CPU_ISSET(cpu,&actual) && CPU_COUNT(&actual)==1);
}
static long flush(unsigned flags)
{
    /* Linux currently ignores start/end, including reversed/unmapped ranges. */
    return syscall(SYS_riscv_flush_icache,UINTPTR_MAX,0,flags);
}
static int (*execute_code)(void);
static volatile uint32_t *write_code;
static void patch(unsigned value)
{
    CHECK(value<2048);
    write_code[0]=(value<<20)|UINT32_C(0x00000513); /* addi a0,zero,value */
}
static unsigned request,ack,ready;
static void *executor(void *unused)
{
    (void)unused;pin(cpu_ids[1]);CHECK(execute_code()==1);
    __atomic_store_n(&ready,1,__ATOMIC_RELEASE);
    /* No syscall between warm-up and execution: this checks an active remote
     * hart, rather than relying on address-space entry to perform FENCE.I. */
    for(unsigned value=2;value<=65;value++) {
        while(__atomic_load_n(&request,__ATOMIC_ACQUIRE)!=value) __asm__ volatile("" ::: "memory");
        CHECK(execute_code()==(int)value);
        __atomic_store_n(&ack,value,__ATOMIC_RELEASE);
    }
    return 0;
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    cpu_set_t available;CPU_ZERO(&available);CHECK(!sched_getaffinity(0,sizeof(available),&available));
    for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&available))cpu_ids[cpu_count++]=i;
    CHECK(cpu_count>0);pin(cpu_ids[0]);
    CHECK(flush(2)==-1 && errno==EINVAL);
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    CHECK(!mkdir("/tmp",0777) || errno==EEXIST);
    const char *path="/tmp/api8-icache-code";
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0 && !ftruncate(fd,page));
    write_code=mmap(0,(size_t)page,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    void *executable=mmap(0,(size_t)page,PROT_READ|PROT_EXEC,MAP_SHARED,fd,0);
    CHECK(write_code!=MAP_FAILED && executable!=MAP_FAILED && write_code!=(void *)executable);
    execute_code=(int (*)(void))executable;
    write_code[1]=UINT32_C(0x00008067); /* ret */
    patch(1);CHECK(!flush(0) && execute_code()==1);
    if(cpu_count>1) {
        pthread_t thread;CHECK(!pthread_create(&thread,0,executor,0));
        while(!__atomic_load_n(&ready,__ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
        for(unsigned value=2;value<=65;value++) {
            patch(value);CHECK(!flush(0));
            __atomic_store_n(&request,value,__ATOMIC_RELEASE);
            while(__atomic_load_n(&ack,__ATOMIC_ACQUIRE)!=value) __asm__ volatile("" ::: "memory");
        }
        CHECK(!pthread_join(thread,0));
        puts("ICACHE CASE active-remote-execution-64-publications PASS");
        patch(66);CHECK(!flush(1));pin(cpu_ids[1]);CHECK(execute_code()==66);
        puts("ICACHE CASE local-publication-migration-to-previous-executor PASS");
        if(cpu_count>2) {
            pin(cpu_ids[0]);patch(67);CHECK(!flush(1));pin(cpu_ids[2]);CHECK(execute_code()==67);
            puts("ICACHE CASE local-publication-late-cpu-entry PASS");
        }
    } else {
        patch(66);CHECK(!flush(1) && execute_code()==66);
        puts("ICACHE CASE local-only-single-cpu PASS");
    }
    CHECK(!munmap((void *)write_code,(size_t)page) && !munmap(executable,(size_t)page) && !close(fd) && !unlink(path));
    printf("ICACHE PASS cpus=%d\n",cpu_count);return 0;
}
#else
int main(void) { puts("ICACHE SKIP non-RISC-V architecture"); return 0; }
#endif
