# 内存后备对象与多挂载验收

固定依据：`references/linux` commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`；内存对象与 tmpfs 主要对照
`mm/shmem.c`、`Documentation/filesystems/tmpfs.rst`、`fs/inode.c` 和
`lib/{cmdline,kstrtox}.c`，硬链接对照 `fs/namei.c`。实现保持 RV64、QEMU virt、单 hart。

## 多缓存的共同压力 owner

每实例保留 worker、快照、页和错误 owner，分配器只登记共同入口。通知与全局
脏阈值为 O(实例数)，完整快照为 O(总缓存项数)。同一个等待者只等共同一轮的
首个实际释放，或所有参与 worker 完成；等待者引用防止最后实例注销时释放
共同 owner。停止一盘不得清除另一盘回调，干净回收仍禁止进入脏 I/O。

`make test-io-sleep-riscv` 在 legacy/modern × writeback/writethrough 四组合验证：
两个非空缓存的 Cached/Dirty/Writeback 求和、低水位只扣一次、轮转释放、各自
未达脏阈值而合计触发，以及等待期间 peer 注销和 root 先释放的两种交错。
写回最终经过真实 ext4。初始测试只保留一页，导致后端 metadata ENOMEM；
改为仍处于低水位但足够事务工作的配置后，才满足“实际释放进展”的测试前提。
这不意味着 worker 有完整应急池。
