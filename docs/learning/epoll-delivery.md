# epoll 交付、扫描与对象寿命

2026-10-05，基线 `5687377c58dc96adfa1f72f1636a319bbd25b419`。
固定依据是 `references/linux/fs/eventpoll.c`，Linux commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的扫描隔离及逐事件交付。

旧实现先摘 ready 项并消费 ET/解除 ONESHOT，再整体复制事件数组。
同一 RV64 ELF 在固定 Linux 和 BoarOS 上确认：首项 EFAULT 后，Linux
仍交付 ET/ONESHOT，旧 BoarOS 返回零；先成功复制一个完整 event 后 fault，
Linux 返回 1 并保留失败项，旧 BoarOS 返回 EFAULT。提前地址范围检查不能
代替真实复制的提交点，用户复制还可能因文件缺页睡眠。

旧扫描把 item 标为未排队后调用目标 poll。poll 内的唤醒能把它加入全局
ready，LT 回队再次使用同一个 next，形成自环。直接编译旧生产函数的
宿主契约测试，在随后目标失去就绪时，由公开 `kernel_epoll_poll` 观察到
不结束的查询；修正后查询和后续非阻塞等待均结束。这是同步边界模型，
不作为完整客体服务器死锁的复现证据。

现在扫描拥有一批 item 引用和当前目标 OFD pin；回调使用独立 pending 链，
结束时去重合并。每个完整 event 成功写入用户后提交，部分结构不计成功。
用户复制睡眠期间，DEL 立即解绑注册但延后释放；MOD 改变控制代次，
旧 ONESHOT 不能解除新注册。扫描期间新通知有独立代次，不被旧交付消费。
整个等待请求登记在 task，强制退出先归还未交付批次和所有 pin，再回收栈。
没有跨用户复制持有 epoll 锁或把关中断误当成不可能调度。

可重建验证：

```sh
make test-epoll-host
make test-epoll-riscv
make test-files-riscv test-userland-riscv test-stack-usage
```

宿主测试编译完整 `fs/files/epoll.c`，只替换用户复制、OFD、调度和等待
边界，启用 ASan/UBSan。覆盖 fault 重试、通知重入、MOD、DEL、close pin、
fd 复用、扫描中的第二等待者、复制期间通知和退出取消，检查资源回到基线。
真实 RV64 的九组 LT/ET/ONESHOT × fault 位置在同一 ELF 的两侧通过，
并检查 ONESHOT MOD 重装；QEMU 为 11.1.1。并发控制操作和强制取消的
确定性交错由宿主边界模型验证，未把它描述成客体并发压力结果。

本次现有 RV64 文件测试、静态 musl 与动态 pthread 用户态及栈检查通过；
不由此声称完整 ABI、完整恢复矩阵或性能收益。socket poll 的协议推进仍在，
它是后续独立改动；epoll 容器已经能承受扫描期间的同步回调。
