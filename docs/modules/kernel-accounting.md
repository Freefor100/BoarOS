# 任务执行与资源记账

## 时间与所有权

任务保存已结束执行片段的纳秒值；当前片段由实际执行的CPU按硬件counter tick累计，在查询及切栈交接时换算为纳秒。硬件tick与times的100Hz用户tick分别换算。U/S trap和用户返回切换执行模式；IRQ进入/返回保留被打断的模式，嵌套IRQ时间不计入任务用量。模式随任务内核栈恢复，CPU上的执行片段归当前实际栈owner；调度器提前发布next/current后，仍在真正切栈前才转移记账。普通切换和退出丢弃栈都经过该转移。

普通trap入口和最终用户返回在既有IRQ关闭范围内更新本CPU片段；每次转换只读counter并更新当前模式，不取全局调度锁、不逐次换算纳秒。SMP用本CPU写序列发布计数与时间边界，远端查询持调度域固定owner和任务存储，再重试跨越发布的数值快照。写者在序列变化期间只访问常驻对象和counter，不分配、不等待；单CPU省去该序列。RV多核的counter采样另用TIME输入域屏障，保证采样位于奇数序列发布之后、最终序列读取之前；普通内存屏障单独承担payload顺序。切栈仍在调度域内把CPU片段归入旧task并转移owner，实时预算的远端结算独立处理其调度状态。

阻塞、排队和睡眠没有执行片段。CPU0实际执行的用户服务计入请求任务内核时间，独立worker的运行留在worker。查询包含正在CPU上运行的尚未结算片段；times和proc转换为100Hz用户tick，rusage转换为微秒。执行计数与调度预算共用同一单调时间源，预算保持其原有语义。

每任务的usage只保存自身统计。线程组另保存已退出线程的dead_usage和已回收子进程的child_usage：线程退出归入dead_usage，消费子进程时归入父组child_usage。组长提前退出及非组长exec沿同一身份转移规则处理；THREAD查询保留调用线程自身计数，SELF汇总全组，CHILDREN只含已回收后代。WNOWAIT只观察，消费时才归并。

## 字段来源

- CPU时间来自实际执行区间；minor/major fault仅记录成功的本线程MM缺页，原子发布后在组快照中读取。
- RSS高水位在实际发布用户PTE时更新，正常页与PROT_NONE保护页合计，解除映射保留峰值。MM释放前保存峰值，exec保留进程历史峰值；子进程峰值取最大值。ru_maxrss以KiB输出。
- 共同块层提交逻辑块读时按字节记给原调用者，分区转换和loop同样适用；磁盘缓存页从干净变为脏时记录一页写入，重复修改同一脏页不重复累计。两项除以512得到ru_inblock/ru_oublock，后台执行沿已有account_task借用归属；tmpfs不产生磁盘脏页记账。同步块OFD的字节写在成功提交后按实际扇区范围计写入。
- 在实际切出时计上下文切换；阻塞、停止及退出计voluntary，仍可运行的切出计involuntary。没有切换的yield不增加次数。
- Linux保留为零的ixrss/idrss/isrss/nswap/msgsnd/msgrcv/nsignals字段保持零。

时间快照把task已完成片段与CPU当前片段相加；线程组快照复用游标及代次重查。MM高水位读取在CPU0、IRQ关闭及既有MM owner覆盖下完成，不等待、不分配；MM和调度域分别取锁。

## 用户接口与验证

RV64/LA64的getrusage编号为165，支持RUSAGE_SELF(0)、THREAD(1)、CHILDREN(-1)。who按32位有符号参数解释，非法who先返回EINVAL，再处理输出地址。原生rusage为144字节；输出fault返回EFAULT并保留已复制前缀。

wait4、waitid使用共同资源快照。退出事件在输出前消费；状态/rusage/siginfo的既有复制顺序保持，复制失败不回滚回收。等待输出包括该子进程已回收后代的用量。

```sh
make test-resource-abi-riscv test-resource-abi-loongarch
make test-resource-smp-riscv QEMU_RISCV_FIRMWARE=/absolute/fw_dynamic.bin
make test-sched-concurrency-host test-user-smp-host
```

raw syscall对照检查返回值、字段单位、线程隔离、后代累计、睡眠排除、RSS保护/解除映射、冷热I/O及输出fault前缀。动态计数比较操作前后关系，保留原值，不把两台独立运行的实际CPU时间逐字比较。RV两核入口要求两个CPU实际运行用户代码、系统调用和本地timer返回，并检查正常退出、资源归还和fsck。

## CPU 时钟

clock_gettime/getres 支持 CLOCK_PROCESS_CPUTIME_ID(2)、CLOCK_THREAD_CPUTIME_ID(3)，以及 Linux 编码的 PROF、VIRT、SCHED 进程/线程时钟。VIRT 读取用户时间，PROF/SCHED 读取用户与内核时间之和；IRQ 和睡眠按上面的执行规则排除。SCHED 分辨率为 1 ns，PROF/VIRT 为 100 Hz tick。[VIRTUAL/PROF间隔定时器](kernel-time.md#进程-cpu-间隔定时器)复用此统计；CPU时间睡眠与POSIX定时器另列在能力目录。

clockid_t 按有符号 32 位解释。PID 0 选择自身；线程时钟限定同一线程组。进程时钟接受 TGID，gettime 额外接受调用线程自己的 TID，getres 沿 Linux 的权限校验规则只接受 TGID。非法编码或消失的目标返回 EINVAL，合法目标的输出地址错误返回 EFAULT；getres 允许空输出。

查询在发起 CPU 读取数值，不借用远端 MM。PID 引用固定进程身份，task 存储借用覆盖快照；两者各自释放。目标被回收时返回 EINVAL，其他线程 exec 将同一个 TGID 转交新容器时，放弃旧容器快照并重取新容器。已退出非主线程的清理存储仍可保留，其线程时钟已结束。


```sh
make test-usage-abi-riscv test-usage-abi-loongarch TEST_MEMORIES="512M 1G"
make test-usage-smp-riscv test-cpu-clock-process-smp-riscv QEMU_RISCV_FIRMWARE=/absolute/fw_dynamic.bin
```

CPU 时钟差分检查编码、进程/线程归属、消失的 TID、zombie/reap、执行增长、睡眠排除和输出地址。组合测试在真实目标存储借用之后暂停查询，分别强制父进程回收及非主线程 exec，再检查结果、实际借用释放和正常磁盘检查。

程序耗时影响、同批三版本对照及复测方法见[CPU记账成本](../learning/cpu-accounting-cost.md)。
