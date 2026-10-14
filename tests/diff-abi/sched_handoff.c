#include "abi.h"
struct handoff_time { long sec, ns; };
struct handoff_shared { volatile unsigned count, stop; };
static __attribute__((noinline)) unsigned long now(void)
{
    struct handoff_time t;
    abi_require(SC2(113, 1, &t) == 0);
    return (unsigned long)t.sec * 1000000000UL + (unsigned long)t.ns;
}
static void policy(long pid, int kind, int priority)
{ abi_require(SC3(119, pid, kind, &priority) == 0); }
static void pause_ns(long ns)
{
    struct handoff_time delay = {0, ns};
    abi_require(SC2(101, &delay, 0) == 0);
}
static void phase(int kind)
{
    long map = SC6(222, 0, ABI_PAGE_SIZE, 3, 0x21, -1, 0);
    abi_require(map >= 0);
    struct handoff_shared *shared = (void *)map;
    shared->count = shared->stop = 0;
    int gate[2], progressed[2];
    abi_require(SC2(59, gate, 0) == 0 && SC2(59, progressed, 0) == 0);
    long child = SC5(220, 17, 0, 0, 0, 0);
    abi_require(child >= 0);
    if (!child) {
        char byte;
        abi_require(SC1(57, gate[1]) == 0 && SC1(57, progressed[0]) == 0);
        abi_require(SC3(63, gate[0], &byte, 1) == 1);
        abi_require(SC1(57, gate[0]) == 0);
#ifdef ABI_HANDOFF_DELAY_NS
        /* 定点反例：子任务延迟不应变成父任务的就绪假设。 */
        pause_ns(ABI_HANDOFF_DELAY_NS);
#endif
        shared->count++;
        abi_require(SC3(64, progressed[1], "r", 1) == 1);
        abi_require(SC1(57, progressed[1]) == 0);
        while (!shared->stop) {
            shared->count++;
            pause_ns(20000000);
        }
        abi_exit(0);
    }
    abi_require(SC1(57, gate[0]) == 0 && SC1(57, progressed[1]) == 0);
    policy(child, 1, 10);
    policy(0, kind, 50);
    /* 首次clock输出可能触发COW/缺页并阻塞；放行子任务前准备。 */
    if (kind == 2) (void)now();
    abi_require(SC3(64, gate[1], "g", 1) == 1);
    if (kind == 1) {
        for (unsigned i = 0; i < 2000; i++) abi_require(SC0(124) == 0);
    } else {
        /* 超过100ms RR片，但低优先级任务仍不能获得资格。 */
        unsigned long stop = now() + 160000000UL;
        while (now() < stop) { }
    }
    abi_require(shared->count == 0);
    /* 真正阻塞高优先级父任务，等待子任务执行后提交的确认。 */
    char ready;
    abi_require(SC3(63, progressed[0], &ready, 1) == 1 && ready == 'r');
    abi_require(shared->count != 0);
    shared->stop = 1;
    policy(0, 0, 0);
    int status = -1;
    abi_require(SC4(260, child, &status, 0, 0) == child && status == 0);
    abi_require(SC1(57, gate[1]) == 0 && SC1(57, progressed[0]) == 0);
    abi_require(SC2(215, map, ABI_PAGE_SIZE) == 0);
    abi_record(kind == 1 ? "sched.fifo-handoff" : "sched.rr-handoff", 0, -1, -1, 0, 0, 0);
}
void abi_sched_handoff_cases(void)
{
    unsigned long original_mask=0, cpu0=1;
    abi_require(SC3(123,0,sizeof(original_mask),&original_mask)==sizeof(original_mask));
    abi_require(SC3(122,0,sizeof(cpu0),&cpu0)==0);
    long mkdir = SC3(34, -100, "/proc", 0755);
    abi_require(mkdir == 0 || mkdir == -17);
    long mount = SC5(40, "proc", "/proc", "proc", 0, 0);
    abi_require(mount == 0 || mount == -16);
    long control = abi_open("/proc/sys/kernel/sched_rt_runtime_us", 1);
    abi_require(control >= 0 && SC3(64, control, "-1\n", 3) == 3 && SC1(57, control) == 0);
    phase(1);
    phase(2);
    abi_require(SC3(122,0,sizeof(original_mask),&original_mask)==0);
}
