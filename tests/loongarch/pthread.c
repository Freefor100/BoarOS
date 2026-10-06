/* Reuse the portable cases verbatim; the RV dynamic/network catalogue remains
 * in its original entrypoint and is not claimed by this static LA runner. */
#define main rv_pthread_catalogue_main
#include "../userland/pthread.c"
#undef main
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

int user_register_probe(void);
static _Thread_local int local_value=31;
static _Thread_local unsigned char local_bss[257];
static pthread_barrier_t cpu_gate;
static atomic_ulong progress[2];
struct cpu_case { unsigned index; int *error_address; void *tls_address; pid_t tid; };
static void *cpu_worker(void *opaque)
{
    struct cpu_case *test=opaque;
    unsigned i=test->index;
    if(local_value!=31 || local_bss[256]) return (void *)1;
    local_value=100+i;local_bss[256]=i+1;errno=70+i;
    test->error_address=&errno;test->tls_address=&local_value;test->tid=syscall(SYS_gettid);
    pthread_barrier_wait(&cpu_gate);
    /* Each worker must see its peer progress while both remain in pure
     * compute loops; no yield or blocking call can supply fairness. */
    for(unsigned long n=0;n<2000000;n++) atomic_fetch_add_explicit(&progress[i],1,memory_order_relaxed);
    while(!atomic_load_explicit(&progress[1-i],memory_order_relaxed)) {}
    if(user_register_probe() || local_value!=100+(int)i || local_bss[256]!=i+1 || errno!=70+(int)i)
        return (void *)2;
    return 0;
}
static int check_cpu_tls(void)
{
    pthread_t threads[2];struct cpu_case cases[2]={{.index=0},{.index=1}};
    if(pthread_barrier_init(&cpu_gate,0,3)) return 1;
    local_value=90;errno=60;
    for(unsigned i=0;i<2;i++) if(pthread_create(&threads[i],0,cpu_worker,&cases[i])) return 2;
    pthread_barrier_wait(&cpu_gate);
    for(unsigned i=0;i<2;i++) {void *result;if(pthread_join(threads[i],&result) || result || atomic_load(&progress[i])!=2000000)return 3;}
    if(local_value!=90 || errno!=60 || cases[0].tls_address==cases[1].tls_address ||
       cases[0].tls_address==&local_value || cases[0].error_address==cases[1].error_address ||
       cases[0].error_address==&errno || cases[0].tid==cases[1].tid || cases[0].tid==getpid() || cases[1].tid==getpid())return 4;
    return pthread_barrier_destroy(&cpu_gate);
}
static int check_nonpi_raw_exit(void)
{
    for(int mode=ROBUST_LIST_OWNER;mode<=ROBUST_BAD_HEAD;mode++) {
        int result=robust_raw_exit_case(mode,0);if(result)return 10*mode+result;
    }
    if(robust_raw_exit_case(ROBUST_COW,0) || robust_raw_exit_case(ROBUST_BAD_OFFSET,0))return 80;
    int woken=0;
    if(robust_raw_exit_case(ROBUST_PENDING_UNLOCK_WAKE,&woken) || woken!=1)return 81;
    for(int tries=0;tries<20;tries++) {
        if(robust_raw_exit_case(ROBUST_WAIT_OWNER,&woken))return 82;
        if(woken==1)return 0;
    }
    return 83;
}
int main(int argc,char **argv)
{
    if(argc==3 && !strcmp(argv[1],"execed")) return execed_mode(argv[2]);
    const struct {const char *name;int (*check)(void);} cases[]={
        {"CPU/registers/TLS/errno",check_cpu_tls},
        {"executable TLS",check_executable_tls},
        {"mutex/condition/barrier/timeouts",check_synchronization},
        {"cancellation/cleanup",check_cancellation},
        {"shared FD blocked owner",check_shared_fd_pin},
        {"leader exit/nonleader exec/group exit",check_thread_lifecycle},
        {"futex/restart",check_raw_futex},
        {"robust registration",check_robust_registration},
        {"robust remote query",check_robust_remote_query},
        {"robust owner death",check_robust_mutex_protocol},
        {"robust non-PI raw exit/COW",check_nonpi_raw_exit},
    };
    for(size_t i=0;i<ARRAY_SIZE(cases);i++) {
        int result=cases[i].check();
        if(result) {fprintf(stderr,"LA static pthread %s failed=%d errno=%d\n",cases[i].name,result,errno);return 1;}
        printf("LA static pthread %s passed\n",cases[i].name);fflush(stdout);
    }
    if((mkdir("/dev",0755) && errno!=EEXIST) ||
       (mknod("/dev/null",S_IFCHR|0666,makedev(1,3)) && errno!=EEXIST))return 2;
    pid_t shell=fork();if(shell<0)return 2;
    if(!shell) {
        execl("/busybox","busybox","ash","-c",
            "trap 'v=signal' USR1; v=unset; kill -USR1 $$; test \"$v\" = signal || exit 1; /busybox sleep 0.02 & p=$!; wait \"$p\" || exit 2; echo 'LA original BusyBox ash signal/wait passed'",(char *)0);
        _exit(3);
    }
    int shell_status;
    if(waitpid(shell,&shell_status,0)!=shell || !WIFEXITED(shell_status) || WEXITSTATUS(shell_status)) {
        fprintf(stderr,"LA BusyBox ash status=%x errno=%d\n",shell_status,errno);return 4;
    }
    puts("LA static pthread catalogue passed");return 0;
}
