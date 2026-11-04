/* Reuse the established completed workloads for the scalar-path comparison. */
#define LA_SMP_COST_EMBEDDED 1
#include "la-smp.c"
#include <sys/mount.h>
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    CHECK(!mkdir("/proc",0755) || errno==EEXIST);
    CHECK(!mount("proc","/proc","proc",0,NULL));
    char configuration[]="1 1";
    FILE *config=fmemopen(configuration,sizeof(configuration)-1,"r");
    CHECK(config && !stack_cost_main(config));
    futex_perf(1);
    filesystem_cost(1);
    CHECK(!umount("/proc"));
    puts("API8-PERF PASS");return 0;
}
