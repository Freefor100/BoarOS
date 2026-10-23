/* Keep the existing work and completion checks; omit unrelated file windows. */
#define main unused_posix_cost_main
#include "../../accounting/posix.c"
#undef main

int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    FILE *config=fopen("/account-posix","r");
    unsigned kind,cpus;
    CHECK(config && fscanf(config,"%u %u",&kind,&cpus)==2 && !fclose(config));
    CHECK(!kind && (cpus==1 || cpus==2 || cpus==4));
    config=fopen("/account-altstack","r");
    CHECK(config && !stack_cost_main(config));
    futex_perf(cpus);
    puts("SIGNAL-FUTEX-PERF END 5");
    return 0;
}
