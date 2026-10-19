#include "abi.h"

struct core_limit { unsigned long current, maximum; };
extern long abi_clone_entry(long,void *,int *,void (*)(void *),void *);
static unsigned char core_stack[16384] __attribute__((aligned(16)));
struct core_worker { int mode, done, ready, leader_alive; long value; };

static long core_get(long pid, struct core_limit *value)
{ return SC4(261,pid,4,0,value); }
static long core_set(long pid, unsigned long value)
{
    struct core_limit limit = {value,~0UL};
    return SC4(261,pid,4,&limit,0);
}
static void core_check(const char *name, int okay)
{ abi_record(name,okay,-1,-1,0,0,0); }
static void wait_zero(int *address)
{
    int value;
    while ((value=__atomic_load_n(address,__ATOMIC_ACQUIRE))) {
        long result=SC6(98,address,0,value,0,0,0);
        abi_require(result==0 || result==-11 || result==-4);
    }
}
void abi_core_limit_exec_probe(void)
{
    struct core_limit value;
    abi_exit(!core_get(0,&value) && value.current==12345 && value.maximum==~0UL ? 57 : 91);
}
static void core_worker(void *argument)
{
    struct core_worker *work=argument;
    if (work->mode==0) {
        abi_require(!core_set(0,12345));
        __atomic_store_n(&work->done,1,__ATOMIC_RELEASE);
        return;
    }
    if (work->mode==1) {
        const char *argv[]={"/init","core-limit-exec",0};
        const char *env[]={0};
        SC3(221,argv[0],argv,env);
        abi_exit(92);
    }
    __atomic_store_n(&work->ready,1,__ATOMIC_RELEASE);
    wait_zero(&work->leader_alive);
    struct core_limit value;
    SC1(94,!core_get(0,&value) && value.current==12345 ? 57 : 93);
    abi_exit(94);
}
void abi_core_limit_cases(void)
{
    struct core_limit saved={0}, value={0};
    long result=core_get(0,&saved);
    abi_record("core.entry",result,-1,-1,0,0,0);
    abi_require(!result);
    core_check("core.default",saved.current==0 && saved.maximum==~0UL);
    abi_record("core.output-fault",SC4(261,0,4,0,(void *)-1),-1,-1,0,0,0);
    abi_record("core.input-fault",SC4(261,0,4,(void *)-1,0),-1,-1,0,0,0);
    struct core_limit bad={2,1};
    abi_record("core.invalid-range",SC4(261,0,4,&bad,0),-1,-1,0,0,0);
    abi_record("core.invalid-resource",SC4(261,0,16,0,&value),-1,-1,0,0,0);
    abi_record("core.missing-target",SC4(261,0x7fffffff,4,0,&value),-1,-1,0,0,0);
    abi_record("core.input-before-target",SC4(261,0x7fffffff,4,(void *)-1,0),-1,-1,0,0,0);
    struct core_limit update={42,~0UL};
    result=SC4(261,0,4,&update,(void *)-1);
    abi_require(!core_get(0,&value));
    abi_record("core.commit-before-output-fault",result,value.current,-1,0,0,0);
    abi_require(!SC4(261,0,4,&saved,0));
    struct core_worker work={0}; int tid_address=0;
    long tid=abi_clone_entry(0x10f00,core_stack+sizeof(core_stack),&tid_address,core_worker,&work);
    abi_require(tid>0);
    while (!__atomic_load_n(&work.done,__ATOMIC_ACQUIRE)) SC0(124);
    while (SC2(130,tid,0)==0) SC0(124);
    abi_require(!core_get(0,&value));
    core_check("core.thread-shared",value.current==12345);
    long child=CALL(220,17,0,0,0,0,0);abi_require(child>=0);
    if (!child) {
        abi_require(!core_get(0,&value) && value.current==12345 && !core_set(0,7));
        abi_exit(57);
    }
    int status=0;abi_require(SC4(260,child,&status,0,0)==child && !core_get(0,&value));
    abi_record("core.fork-isolation",value.current==12345,-1,-1,status,0,0);
    child=CALL(220,17,0,0,0,0,0);abi_require(child>=0);
    if (!child) {
        const char *argv[]={"/init","core-limit-exec",0};const char *env[]={0};
        SC3(221,argv[0],argv,env);abi_exit(92);
    }
    abi_require(SC4(260,child,&status,0,0)==child);
    abi_record("core.exec-inherit",0,-1,-1,status,0,0);
    child=CALL(220,17,0,0,0,0,0);abi_require(child>=0);
    if (!child) {
        work=(struct core_worker){.mode=1};
        abi_require(abi_clone_entry(0x10f00,core_stack+sizeof(core_stack),&tid_address,core_worker,&work)>0);
        for (;;) SC0(124);
    }
    abi_require(SC4(260,child,&status,0,0)==child);
    abi_record("core.nonleader-exec",0,-1,-1,status,0,0);
    child=CALL(220,17,0,0,0,0,0);abi_require(child>=0);
    if (!child) {
        work=(struct core_worker){.mode=2,.leader_alive=1};
        abi_require(SC1(96,&work.leader_alive)>0);
        abi_require(abi_clone_entry(0x10f00,core_stack+sizeof(core_stack),&tid_address,core_worker,&work)>0);
        while (!__atomic_load_n(&work.ready,__ATOMIC_ACQUIRE)) SC0(124);
        SC1(93,0);abi_exit(95);
    }
    abi_require(SC4(260,child,&status,0,0)==child);
    abi_record("core.leader-exit",0,-1,-1,status,0,0);
    child=CALL(220,17,0,0,0,0,0);abi_require(child>=0);
    if (!child) { for (;;) SC0(124); }
    abi_require(!core_set(child,999) && !core_get(child,&value));
    core_check("core.other-target",value.current==999);
    abi_require(!SC2(129,child,9) && SC4(260,child,&status,0,0)==child);
    abi_require(!SC4(261,0,4,&saved,0) && !core_get(0,&value));
    core_check("core.restore",value.current==saved.current && value.maximum==saved.maximum);
}
