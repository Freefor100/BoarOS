#include <stdint.h>
#include "../common/raw_syscall.h"
struct sched_param { int priority; };
extern int sched_getparam(int, struct sched_param *);
extern int sched_getscheduler(int);
extern int sched_setparam(int, const struct sched_param *);
extern int sched_setscheduler(int, int, const struct sched_param *);
extern int *__errno_location(void);

static void message(const char *s)
{
    unsigned n=0; while(s[n])n++;
    test_syscall6(64,1,(long)s,n,0,0,0);
}

int user_main(uint64_t *stack)
{
    (void)stack;
    struct sched_param parameter={-1};
    int *error=__errno_location();
    *error=123;
    if(sched_getparam(0,&parameter) || parameter.priority!=0 || *error!=123)
        return 91;
    if(sched_getscheduler(0)!=0 || *error!=123) return 92;
    parameter.priority=0;
    if(sched_setparam(0,&parameter) || sched_setscheduler(0,0,&parameter)) return 93;
    *error=0;
    if(sched_getparam(0,(void *)1)!=-1 || *error!=14) return 94;
    *error=0;
    if(sched_getscheduler(2147483647)!=-1 || *error!=3) return 95;
    *error=0;
    if(sched_setparam(0,(void *)1)!=-1 || *error!=14) return 96;
    *error=0;
    if(sched_setscheduler(0,99,&parameter)!=-1 || *error!=22) return 97;
    message("RUNTIME SCHED four interfaces, preserved errno, EFAULT/ESRCH/EINVAL passed\n");
    return 0;
}
