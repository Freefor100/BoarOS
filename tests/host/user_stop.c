/* 静态生产停止操作由测试入口暴露；调度锁/claim/通知保持真实实现。 */
#include "../../kernel/sched/signal.c"
void user_stop_case(struct kernel_task *task) { signal_stop_member(task,SIGNAL_STOP); }
/* 此反例目标先READY再RUNNING，不执行阻塞信号后端。 */
enum kernel_scheduler_status __wrap_kernel_scheduler_wake_signal(struct kernel_task *task)
{ (void)task; __builtin_trap(); }
