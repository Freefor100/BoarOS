# IPC 命名空间

`kernel/ipc_namespace.c` 维护 IPC 命名空间的身份及四类 IPC 发现状态。
任务与命名空间 FD 持有通用 `kernel_namespace` 引用；初始命名空间另有永久启动引用。
关机在任务、文件和挂载释放后，通过 `kernel_ipc_initial_shutdown` 归还初始空间的实际 IPC 表，保留启动身份；仍有外部空间引用时返回 EBUSY 并保留全部表。
`CLONE_NEWIPC` 创建空空间，普通 fork/clone 继承原空间；`setns` 安装指定空间。
任务切换空间前完成所有准备，提交时先脱离原 `SEM_UNDO` 共享组，再替换任务引用。
clone 的 `CLONE_NEWIPC|CLONE_SYSVSEM` 组合不兼容；unshare 的 `CLONE_SYSVSEM` 则表示脱离共享撤销组。运行时 IPC sysctl 不在此接口范围内。

## 发现与对象存活

SysV SHM、信号量和消息队列分别按命名空间维护 key、ID 槽、代次及使用量。
空空间查询和 INFO 不分配完整表；第一次创建才分配相应表。
相同数字 key 或 ID 可以在不同空间指向不同对象。系统调用从实际任务借用空间，
`/proc/sysvipc/{shm,sem,msg}` 在打开时取得空间引用，后续读取与重新定位继续查询该空间。

信号量集合与消息队列保留原表引用，请求及撤销记录保留原对象。
命名空间最后释放时通过普通删除所用的内部实现移除发现项、唤醒等待者并归还数据，
无需伪造有管理权限的调用者。表等最后一个真实集合或队列持有者释放后回收。

SHM attachment 持有原命名空间和内存对象。切换空间后，旧 VMA 的 fork、分裂、mremap、
解除附加继续操作原 attachment；不会用新空间中的相同 shmid 查找旧段。
RMID 段只要仍存活，就允许按完整 ID 再次 shmat；key 已撤销，最后 attachment 释放后销毁。

## POSIX 消息队列树

每个 IPC 空间拥有独立 MQ 树，树维护名称、inode、根目录元数据及队列数。
同一空间的所有 mqueue 挂载与 mq_open 共享这棵树及设备身份；不同空间的同号 inode 不会具有相同的设备身份。
旧挂载和队列可以比命名空间活得更久，因此它们持有树引用，不永久引用命名空间。
最后一个命名空间引用消失时，在树锁下清除反向弱关联；旧挂载仍可访问已有项，
创建新队列返回 EACCES。最后一个挂载及命名空间树 owner 消失时撤销名称，
已打开 FD 继续正常收发、poll 和通知，直到最后队列引用释放。

传统 mount 使用调用时的 IPC 空间，fsopen("mqueue") 在打开 context 时捕获空间，
即使调用者在 FSCONFIG_CMD_CREATE 前切换空间，实例仍属于捕获的原空间。
队列数量按空间维护；RLIMIT_MSGQUEUE 创建额度和 SHM_LOCK 的锁页额度仍记在真实 UID
账户上，不因切换空间而重置，也不因修改文件属主而转移。

## 行为检查

`tests/host/{sem,msg,mqueue}.sh` 在 4/16 KiB 页配置下使用真实 namespace、对象、raw lock
及停车实现，检查空表不分配、相同 key/ID 隔离、旧对象的原表回收、MQ 旧挂载和旧 FD
存活，以及通知和 UID 额度归还。SHM 查询和锁页检查见 `tests/host/shm-lock.sh`。
用户态消费者 `tests/workloads/resource/ipc_namespace.c` 检查 namespace FD、切换、clone、
旧 SHM 映射、MQ 挂载及 fsopen context 的真实系统调用行为。

命名空间及 mqueue 生命周期依据固定的 `references/linux/ipc/namespace.c`、`mqueue.c`
和 `util.c`；版本由 [参考资料清单](../../references/sources.tsv) 统一维护。
