# System V 信号量

## 接口与执行位置

RV64和LA64接入原生Linux的semget(190)、semctl(191)、semtimedop(192)、semop(193)。
一个集合包含多个计数器；操作可以增减某项，或等待某项为零。一次传入的整个操作数组共同成功，失败或阻塞时不保留前缀。原glibc的semop经semtimedop执行，musl使用semop入口，两者共用同一实现。

集合、权限、数值检查、等待登记及返回在调用CPU执行。ID/key索引由短保护维护，各集合独立保护数值、请求和撤销记录；数值变化者在该集合内处理等待请求，发布确定结果后锁外通知，不新增CPU0业务交接或worker。

semctl支持IPC_SET/STAT/RMID、GETVAL/SETVAL、GETALL/SETALL、GETPID、GETNCNT/GETZCNT、IPC_INFO、SEM_INFO和SEM_STAT/ANY。IPC权限使用effective身份、补充组、creator/owner和CAP_IPC_OWNER/CAP_SYS_ADMIN，与共享内存共用规则。固定ABI和检查顺序依据references/linux/ipc/sem.c及对应UAPI；64位nsops寄存器按Linux的unsigned int参数截断。

默认上限为32000个集合、每集合32000项、总计1024000000项，每次最多500个操作。计数范围0～32767，撤销调整范围-32768～32767。集合、成员和撤销数组按需分配；32000槽的16位代次表使用静态64000字节。INFO返回真实限额和当前使用量。集合在IPC_RMID前持续存在，程序退出不会自动删除一个普通集合。

## 对象、等待与撤销

命名集合使用key索引及带代次的semid；查找取得存储引用后才离开索引保护。RMID撤下索引并标记删除，等待请求返回EIDRM；集合存储等待真实请求和撤销记录归还后释放。最后put、用户复制和分配均在短保护外。

操作数组只导入一次，重复成员按顺序计算。在集合保护内暂时应用数组，遇到失败则逆序撤销；sempid和时间只在成功后发布。超时和信号分别返回EAGAIN、EINTR，不自动重启。GETNCNT/GETZCNT统计请求当前首先阻塞的操作。完成、超时、信号与删除共用终态，通知持有请求和任务借用直到结束，覆盖完成早于休眠及远端返回的交错。

调用期请求登记到实际任务，持有导入数组、集合、TGID身份和撤销记录。输入尚未导入完、尚未取得集合的请求也可由退出清理回收。标量semctl直接处理受保护状态；STAT先取得快照并归还集合，再复制用户内存；GETALL/SETALL使用可清理的数组存储，避免把大数组压入内核栈。

SEM_UNDO在独立共享组中按集合保存反向调整。任务各持一份组引用，CLONE_SYSVSEM在发布child前共享，包括父线程尚无调整的情况；不带该标志的fork不继承调整。exec保留执行线程自己的组，非组长exec只变更进程身份，不接管旧组长的撤销组。最后一个共享者退出时应用补偿，数值限制在0～32767，随后才发布退出结果。失败构造和重复清理消费已有引用，不重复补偿。

SETVAL清除所有组对该项的调整，SETALL清除该集合全部调整；RMID摘除关联并归还记录。集合保护、组成员保护和实际引用共同覆盖RMID与最后共享者退出的交错。线程的TGID从实际进程组身份取得，清理者使用被退出任务的身份。

`/proc/sysvipc/sem` 与 `semctl` 共用真实集合描述符，按 slot 输出当前集合及操作、修改时间；读取所有权和重读规则见 [procfs 模块](procfs.md)。消息队列由独立的 [SysV 消息模块](sysv-msg.md) 提供。当前未提供 IPC namespace、运行时 IPC sysctl 和 unshare 扩展。

## 聚焦验证与应用

```sh
sh tests/host/sem.sh
python3 tests/network_runner.py --workload sysv_sem --only boaros --cpus 4 --memory 512M --kernel build/riscv-user-smp4/kernel-rv --output build/sem-rv4
python3 tests/sysv-log.py --arch riscv --output build/sem-log-rv
python3 tests/sysv-log.py --arch loongarch --output build/sem-log-la
```

宿主用真实集合、raw和等待代码，在4/16KiB页配置检查原子回滚、配额、ENOMEM、通知、弃栈及RMID/退出的明确交错。用户消费者检查Linux ABI、两种libc、线程和进程共享、fork/exec/退出，以及每个实际客核的修改、等待和补偿；FD和集合数量必须归还。

共享日志消费者复用固定源码的完整BusyBox ELF，不替换程序：syslogd -C经命名UNIX DGRAM接收logger消息，写入SysV共享内存；两个真实logread同时读取并校验内容。另让一个读者持有SEM_UNDO计数，确认真实syslogd已经等待该计数为零，再终止读者，检查退出补偿解除写入阻塞。daemon退出后检查集合和共享内存已删除。该BusyBox是项目已有构建物，原评测盘ELF另行记录。
