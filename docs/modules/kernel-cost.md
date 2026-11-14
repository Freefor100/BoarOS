# 成本观测模块

`COST_DIAGNOSTICS=1` 在实际业务边界记录工作次数、等待、运行时间与设备提交量，默认构建不生成这些采样。它用于解释开销归属；吞吐和延迟比较仍使用关闭诊断的内核。

## 构建与控制

RV 默认观测内核位于 `build/cost/kernel-rv`；USER_SMP 观测版本使用 `build/cost-smp<容量后缀>/`。C 与汇编共用 `BOAROS_COST_DIAGNOSTICS`，配置改变时由生成头文件使相关对象重新编译。任务诊断字段和固定聚合容量均由编译断言检查，实际大小随快照输出。

观测内核提供 `/proc/boaros_cost_control` 和 `/proc/boaros_cost`：

- 向控制节点写入完整 `begin\n` 开始窗口，写入 `end\n` 结束窗口。开始和结束均在发起线程返回用户态的边界完成，其他核的返回不能代替它。
- 窗口归发起线程组及其后代；fork/clone 继承窗口身份，exec 保留。后台执行者的工作另行归类，不能把写者线程的 CPU 时间当成全部内核成本。
- 重复开始返回 EBUSY；其他控制组结束返回 EPERM；仍有在途操作时结束返回 EBUSY。调用者需等待实际操作结束，不能清零在途状态冒充完整窗口。
- 控制组退出使窗口中止并标为 incomplete。控制命令先完成用户输入复制，再改变窗口状态；复制故障不会提交半条命令。
- 快照在结束或中止后形成；运行时读取返回 EBUSY。生成文本、短读、seek 和用户输出故障沿 proc 快照机制处理。

诊断对象只记录标量身份和计数，不另持有任务、MM、inode 或 OFD。聚合使用独立保护，热路径不分配、打印或休眠；每核保存陷入、中断和用户返回边界。普通构建仍保留业务本来需要的请求数、字节数和期限，不保留仅用于观测的时钟读取。

## 快照与归因

缺页观察使用 `mm_fault_ticks` 记录共同解析器，包含后备页准备、等待与提交；
`mm_cow_copy_ticks` 只记录实际私有页复制，`mm_page_alloc_ticks` 只覆盖分配器调用。
`mm_sync_ticks` 覆盖显式提交及预装尾部同步，不含激活 MM 或 spurious fault 的直接本地刷新。
`mm_activate_instruction_flushes`只计进入MM时实际执行的本地指令同步，不含通用发布路径或固件内的其他同步；
`file_query_ticks` 覆盖 stat/statx 的路径导入、查询及结果输出；`path_fallback_ticks`
记录需要从魔术链接实际对象取得属性或重新打开的额外处理；`rfence_calls` 为真实 SBI
RFENCE 调用数。这些阶段存在嵌套，累计经过时间含等待，不能直接相加为墙钟或纯 CPU。
诊断静态聚合区上限为100KiB，实际占用仍由快照报告，默认构建不包含这些存储与采样。

指标及单位以 `include/kernel/cost.def` 为准，`tests/cost/cost_report.py` 按当前 schema 严格解析。完整字段集、唯一性、单位、直方图与状态不匹配时直接报错；旧实验格式通过对应 Git 版本读取，不在当前解析器中保留历史适配分支。

| 内容 | 含义与使用限制 |
|---|---|
| operation / run / blocked / ready | 分别描述操作经过时间、任务运行、阻塞及就绪等待；操作可包含睡眠和嵌套区间，不能相加成整次墙钟 |
| foreground / background / observer | 分别归属窗口成员、后台执行及观测体；observer 是运行开销的一部分，不能再次加到运行时间 |
| IRQ、raw lock、端点与协议池保护 | 记录实际边界的取得、等待或持有；多任务累计值可以超过整次耗时 |
| cache queue / execute | 区分请求入队至执行和 worker 执行区间；执行区间仍可包含锁或设备等待，不等于纯 CPU 时间 |
| 用户复制、页解析、分配与缓存查询 | 描述实际工作量；字节、引用容量和物理页数不能相加作为驻留峰值 |
| 设备字节与提交阶段 | 按提交身份记录请求量；ordered data、日志正文、commit 与 checkpoint 的具体分类以当前指标为准，请求完成不等于全部数据持久化 |

时钟频率来自实际架构时钟，快照报告分辨率。耗时直方图有 65 桶：零值独立一桶，其余为二的幂区间；据此计算的分位数应报告区间。溢出饱和并设置标记，incomplete、overflow 或未排空的窗口不能用作完整对照。

## 当前消费者

存储和 TCP 使用 `tests/cost/io-budget-experiment.py`、`network-budget-experiment.py` 及实际业务程序；运行方式与完成条件见[数据路径实验工具](../learning/data-path-budget-experiments.md)。CPU、线程及定时器使用 `tests/cost/accounting/cost.py`，见[CPU 测量](../learning/cpu-accounting-cost.md)。早期独立校准器和一次性参数矩阵已删除。

各版本保持同一 ELF、输入、总工作量和同步完成条件，串行正式计时。诊断启动与正式时间分开；计数减少只能说明重复工作下降，应用收益由实际吞吐和延迟确认。当前线程亲和性、测量互斥和种子工具由现用执行器直接调用，不为旧实验保留第二套框架。
