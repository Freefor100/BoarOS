# epoll 交付、扫描与对象寿命

最初交付问题对应 `5687377c58dc96adfa1f72f1636a319bbd25b419`。
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

注册由 epoll 私有 intrusive AVL 按 `(fd, OFD)` 索引。同一 OFD 的不同 dup fd
分别登记；fd 复用后，旧 OFD 仍有其他持有者时，新旧 key 可以共存。
ADD 先分配私有候选，再禁止 CPU0 任务切换，完成最终图检查、重复 key 检查和发布。
图遍历期间允许 IRQ；AVL、目标 OFD 反向登记、ready/pending 的实际链接由短 IRQ
保护。ready/pending 都是可直接摘除的双链，删除无需扫描队列。

poll 和 wait 共用候选刷新：逐项从 ready 链借用当前 item 与目标 OFD，
在保护区外查询目标，再检查控制代次及通知代次。目标已失效且代次未变的项直接摘除；
查询期间发生 MOD 或通知的项留给下一轮。没有持久的 readiness 缓存。
共享子图首次被查询后，失效候选不会留在 ready 链上让另一父路径反复查询。
纯 poll 保留有效候选，不消费 ET 或解除 ONESHOT。

请求只借用一个 `current` 候选，取消直接归还它，不维护另一条恢复链；登记数量
不再另存没有消费者的计数。通知代次继续保护锁外查询：新通知可能只更新已经在
ready 上的同一项，旧的空结果不能随后删除它。`in_scan` 让纯查询识别交付者已经
摘下、尚未完成复制的当前项，并避免清除该交付期间新增的 pending 通知。

纯查询不占事件交付的独占游标，只持有正在查询的 item/OFD 引用；另一个调用
正在交付的候选也可以被观察，不能把游标忙当成没有事件。通常直接清理失效链头；
交付并发或代次变化时，本轮用 AVL key 游标访问其余候选，从最小 key 开始并跳过
已观察的保留项，不跨查询借用树节点。查询既不等待别人的扫描，也不发新通知，
因此 DEL 后反向 ADD 不会把旧借用变成游标互等。实际源通知与交付归还的有效候选
仍沿同一等待队列唤醒任务和父 epoll。

扫描期间的新通知使用独立 pending 链，结束时去重合并；本轮不会重复交付同一个 LT 项。
`maxevents` 很小时不再摘取、引用并重新挂回整条链。
每个完整 event 成功写入用户后提交，部分结构不计成功。
用户复制睡眠期间，DEL 立即解绑注册；当前候选延后释放，未选项目立即归还。
MOD 改变控制代次，旧 ONESHOT 不能解除新注册；新通知不被旧交付消费。
wait 与嵌套纯查询请求沿 task 的单个 top 指针组成栈，正常返回逐个弹出；
强制退出先归还所有扫描和 item/OFD 借用，再回收任务栈。

目标查询使用目标本身的同步域；socket poll 在短 IRQ 保护内读取端点队列、状态和
发送接纳额度，不访问 lwIP PCB 或取得协议 mutex。epoll 不持有跨嵌套图的通用
协议 scope。ADD 在借用目标 OFD 时先查询稳定等待队列，
绑定后再查询实际状态，覆盖订阅前已经结束的通知。
pwait 订阅自己的任务队列后只复查 ready/scan 元数据，再 park/finish。
poll 的 prepared token 阶段对 epoll 只使用非阻塞候选提示；它只决定是否先 finish
再刷新，最终 revents 仍由 token 之外的真实查询产生。已有扫描者时允许在任务队列
等待；扫描恢复候选会唤醒任务，清空候选则没有事件可交付。
最终目标 OFD 关闭和 item 堆释放在 epoll IRQ 保护及协议 scope 外，不能把
FIFO 的 VFS 关闭、页归还或用户复制藏在目标查询的协议 mutex 内。

嵌套 epoll 的通知按一次源队列唤醒合并。等待游标在本 CPU 的 IRQ 保护内为整轮嵌套通知提供序号；每个 epoll 在这一轮只唤醒父队列一次。每条注册仍更新自己的通知代次和 ready/pending 状态，扫描期间到达的事件不被合并掉。下一次源通知取得新序号，即使对象仍有 ready 项也会传播。128 个 epoll、3104 条注册的四层重复路径，单轮回调从 1,082,400 次变为 3104 次；这描述操作数，不是应用加速倍数。

四层、每层 32 个 epoll 的无事件共享 DAG，单顶查询的底层 poll 次数从
32768 次降至 32 次；再次空查询不再调用底层 poll，刷新不产生父回调。
这些是宿主生产函数 fixture 的实测操作数，不是应用吞吐或延迟收益。

可重建验证：

```sh
make test-epoll-host
make test-epoll-riscv
make test-files-riscv test-userland-riscv test-stack-usage
```

宿主测试编译完整 `fs/files/epoll.c`，只替换用户复制、OFD、调度和等待
边界，启用 ASan/UBSan。覆盖 fault 重试、通知重入、MOD、DEL、close pin、
fd 复用、128 个打乱顺序的索引键、分配期间重复 ADD 和反向图插入、扫描中的第二等待者、
复制期间通知、嵌套纯查询取消、纯查询/交付重叠、解绑后反向登记及无序候选的代次重试，检查分配与引用全部归还。
fixture 还检查目标查询处 IRQ 开启且可抢占，并在目标查询边界注入可睡眠 owner；
这是对锁外查询的边界模型，不是生产 socket poll 的保护方式。用户复制和最终
OFD/堆释放不持协议 scope，prepared 候选提示不调用目标 query，
park/finish保持IRQ关闭并在外层恢复原IRQ状态。
原交付改动的真实 RV64 九组 LT/ET/ONESHOT × fault 位置在同一 ELF 的
固定 Linux 与 BoarOS 两侧通过，并检查 ONESHOT MOD 重装；该次 QEMU 为 11.1.1。
索引、共享 DAG 刷新及可睡眠目标查询的直接验证是宿主边界模型，
未将历史客体验证或操作数变化描述为本轮客体并发、完整 ABI 或应用性能结果。
