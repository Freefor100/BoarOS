# 成本观测模块

`COST_DIAGNOSTICS=1` 在 `build/cost/riscv/` 构建观测版本，内核为
`build/cost/kernel-rv`；默认 0。C/assembly 同用 `BOAROS_COST_DIAGNOSTICS`，
每个对象依赖只在配置改变时更新的 `generated/cost-config.h`，包括显式覆盖
`BUILD_DIR` 的构建。关闭时宏为空，任务/VFS 节点布局无观测字段，proc 无观测节点。

`kernel/cost.c` 保持固定聚合，`kernel/sched/cost.c` 连接时钟、任务关系和用户返回。
诊断存储不持有任务、MM、inode 或 OFD 引用；任务仅有标量 epoch、运行/等待时间戳、
在途深度及控制抑制状态（当前 48 字节），原任务元数据页容量断言继续生效。
热路径不分配、不打印、不睡眠；原始 CSR SIE save/restore 保护聚合，不走被观测的锁。

仅观测版本提供 `/proc/boaros_cost_control`（完整 `begin\n`、`end\n`）及只读
`/proc/boaros_cost`。开始在用户返回时激活，结束在用户返回时完成；控制 fd 的完整
read/write/readv/writev/定位读写在 dispatch 前识别并抑制自身计数。重复开始 EBUSY，
状态/命令错误 EINVAL，其他控制组结束 EPERM，在途结束 EBUSY，均不清零原窗口。
控制组退出中止窗口并标 incomplete。完整用户复制先于控制发布，坏指针 EFAULT。
控制节点使用同 OFD offset 串行机制，但完整命令不受当前 offset 限制。

窗口身份是递增 epoch，owner 是进程身份代次。开始覆盖控制线程组和当前后代；
后续 fork/clone 继承 epoch，exec 保留。开始前已阻塞的成员操作重新登记在途数量，
结束不得越过它；取消消耗本任务尚未离开的 scope，未回到的栈不成为永久诊断 owner。
内核 fixture 直接调用同一 begin/end/format，必须传 DTB frequency，并以 mode=fixture 标记。

版本 1 快照是完整固定键值 schema，入口定义在 `include/kernel/cost.def`；每个 metric
提供 unit/value/samples/max，耗时直方图另有 bucket.0–64。0 桶只含零，桶 i 含
`[2^(i-1), 2^i-1]`；分位数只能报告区间。时钟单位为 CSR time ticks，timebase 来自 DTB，
快照给出纳秒分辨率的分数，不把 QEMU 墙钟当硬实时界限。
前台按成员 epoch 归属，其他工作属于后台，计数/直方图更新体另计 observer_ticks。
operation_ticks 是包含睡眠和嵌套的墙上客体时间；run_ticks 在切换/返回时结算。
两者不能相加；观察体 ticks 也只是包含在运行时间中的子集，未计入口/时钟读/恢复指令。
blocked/ready 的实际路径将在 C2 接入，未接入不能由零值推断无等待。

快照只在完成/中止后格式化，由生成式 OFD 持有文本；运行窗口中读取返回 EBUSY。
短读、fault 前缀及 rewind 沿用 proc 快照契约。文本分配或容量失败返回明确错误，
不会改变被测操作的 errno、短写或清理结果。溢出饱和并置标记，报告验收拒绝它。

验证入口：`make test-cost-host`、`make test-cost-riscv COST_CASE=contract`。
后者串行运行三个独立启动副本，在启动前保存 kernel/ELF/fixture 身份与源码内容哈希。
目前交付 contract；未交付 write/locking/mprotect/deadline/latency/consumer 明确失败，
`all` 不跳过缺项。独立报告读器拒绝缺项、重复、未知键、单位错误、旧 epoch、
直方图不一致、incomplete 和 overflow。当前 Python discovery 不收集带连字符的文件，
因此 host target 直接运行 `python3 -B tests/test-cost-report.py`，必须实际执行测试。
