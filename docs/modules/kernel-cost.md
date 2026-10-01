# 成本观测模块

`COST_DIAGNOSTICS=1` 在 `build/cost/riscv/` 构建观测版本，内核为
`build/cost/kernel-rv`；默认 0。C/assembly 同用 `BOAROS_COST_DIAGNOSTICS`，
每个对象依赖只在配置改变时更新的 `generated/cost-config.h`，包括显式覆盖
`BUILD_DIR` 的构建。关闭时宏为空，任务/VFS 节点布局无观测字段，proc 无观测节点。

`kernel/cost.c` 保持固定聚合，`kernel/sched/cost.c` 连接时钟、任务关系和用户返回。
诊断存储不持有任务、MM、inode 或 OFD 引用；任务仅有标量 epoch、运行/等待时间戳、
在途深度及控制抑制状态（当前标量 48 字节，加内嵌 backend guard 的 16 字节，共 64 字节），原任务元数据页容量断言继续生效。
热路径不分配、不打印、不睡眠；原始 CSR SIE save/restore 保护聚合，不走被观测的锁。

仅观测版本提供 `/proc/boaros_cost_control`（完整 `begin\n`、`end\n`）及只读
`/proc/boaros_cost`。开始在用户返回时激活，结束在用户返回时完成；控制 fd 的完整
read/write/readv/writev/定位读写在 dispatch 前识别并抑制自身计数。重复开始 EBUSY，
状态/命令错误 EINVAL，其他控制组结束 EPERM，在途结束 EBUSY，均不清零原窗口。
控制组退出中止窗口并标 incomplete。完整用户复制先于控制发布，坏指针 EFAULT。
控制节点使用同 OFD offset 串行机制，但完整命令不受当前 offset 限制。

consumer 包装器允许同步已达到 durable、后台 checkpoint 尚在途的窗口：被测操作
结束时先记录 `COST RESULT` 的时间边界；`end` 返回 EBUSY 时在用户态有界等待重试，
最多十秒，并另报 `COST CLOSING name ns retries`。关闭等待属于快照覆盖区间，
不能并入原操作延迟，也不能把原操作已成功解释成 checkpoint 已排空。其他受控
case 的严格结束检查不变；内核的 EBUSY/inflight/complete 契约不放宽。

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
C2 在真实 block/wake/ready/switch 路径分别结算阻塞墙上时间、就绪等待和运行时间；
idle/cleanup 上下文单列 idle_ticks，不能解释成精确 WFI 驻留时间。

快照只在完成/中止后格式化，由生成式 OFD 持有文本；运行窗口中读取返回 EBUSY。
短读、fault 前缀及 rewind 沿用 proc 快照契约。文本分配或容量失败返回明确错误，
不会改变被测操作的 errno、短写或清理结果。溢出饱和并置标记，报告验收拒绝它。

验证入口：`make test-cost-host`、`make test-cost-riscv COST_CASE=contract`。
后者串行运行三个独立启动副本，在启动前保存 kernel/ELF/fixture 身份与源码内容哈希。
目前交付 contract/write/locking/mprotect/deadline/latency/consumer，
`all` 不跳过缺项；consumer 不将未完成命令标为完成。独立报告读器拒绝缺项、重复、未知键、单位错误、旧 epoch、
直方图不一致、incomplete 和 overflow。当前 Python discovery 不收集带连字符的文件，
因此 host target 直接运行 `python3 -B tests/test-cost-report.py`，必须实际执行测试。

C1 进一步按 file/stream/dgram/other 记录 calls、requested/accepted、实际 usercopy、页解析、
staging 累计请求容量、heap 请求字节和成功物理页。短写以实际接受前缀为准；同步尾部失败
不会抹掉先前接受量。全局 heap/page 指标是实际 allocator 入口和成功结果，按本层单位记录，
不能把 heap 字节、物理页及 staging 引用容量相加当驻留峰值。
缓存计 bucket probes、实际复制、范围写回的两轮遍历、完整页快照与逻辑后端接受量。
设备按 registry 的磁盘0/1/其他聚合；请求在 submit 保存标量 epoch/lane，IRQ 完成和 reset
使用提交身份，旧请求不能污染新窗口。原设备/MM 统计契约不变，fixture 未登记设备属于 other。
封口事务的提交路径按 ordered data、journal、checkpoint metadata 与 journal superblock 设置标量 phase，设备按真实提交上下文归因；其余字节继续归 unknown_read/unknown_write（含之后失败的请求）。请求字节不表示持久字节；后端逻辑数据不足以证明其他扇区分类。
复制/解析和设备请求由 scale 的独立 wrapper/原统计交叉验证；三个副本及完整窗口由
`tests/cost-summary.py` 再验证。C1 结果和开关开销见[成本基线](../learning/cost-baseline.md)。

C2 按 rank 10/15/20/30/40/other 记录尝试、取得、阻塞、唤醒、重阻塞、取消及等待/持有直方图。
持有 scope 参与在途计量，guard 只新增开始时刻和登记标量，不持有诊断任务引用。
独立假时钟测试区分前台运行20 ticks、后台运行890 ticks、前台睡眠880 ticks和就绪10 ticks。
低内存 io-sleep fixture 使用相同接口输出 pressure-io/timeout-cancel，完成后才分配快照；
名称/单位使用字符数组，避免 fixture 关闭分页后解引用高半区绝对字符串指针。
真实 U-mode 的13个 locking窗口检查同/独立OFD、同/不同inode、1/8/32等待者及双盘，
覆盖冷映射输入、向量追加、O_SYNC和截断。U-mode取消阶段在write前撤销gate等待者；
实际写入在途取消由低内存io-sleep的operation_mode=3保护。四种设备配置各三次 fixture 启动验证清理。

C3 计数覆盖外围VMA查询/覆盖/权限/数组编辑/合并/recount、resident遍历和实际PTE/TLB，
prepare/commit/整次改权为嵌套墙上耗时，不能相加。聚合上限包括名称、单位、索引和桥接标量预留。

C5 使用hart全局标量跟踪实际SIE转换，嵌套不重起，切换不截断；采样段及明确盲区见成本基线。
trap保存t0/t1后首采样，sret恢复其他寄存器后发布尾stamp，下一安全入口聚合。
C save/restore、trampoline、timer启动、idle/trap/sret均接入；诊断原始临界区单列observer。
wake_to_run只含实际wake，ready_ticks另含创建/yield等就绪。关闭后C/ASM都无新增指令。

C6 使用固定原镜像的ELF/脚本/依赖，逐命令保存ELF、argv、cwd、真实wait status、
诊断timeout与原stdout/stderr。解释器路径按原ELF安装，libc目录各自指定；不改ELF或uname。
原脚本成功不作为原命令成功证据；wait=0、无timeout、原完成marker及未报告所选测试不可用四条件同时成立才记所选测试完成。另保留进程完成状态与拒绝原因。
`--consumer-timeout-ms` 显式设置每命令客体预算（1000–3600000 ms，默认仍为180000）；
配置写入启动前封存输入和fixture，同一协调ELF读取它并输出唯一预算header，Python核对一致性。
宿主兜底预算按16条命令预算加120秒计算。放宽预算不会改变上述完成条件，
新配置独立保存，不能覆盖旧180秒基线或冒充原评测3600秒总预算。
`tests/iozone-closure.py` 是续测收口门禁：要求观测开/关及固定Linux各三个独立启动、
相同协调ELF、开/关相同生产源码、相同预算/QEMU/firmware/timebase、
原ELF/脚本/依赖/argv/cwd、完整封存输入和实际客体uname。
两种libc的0–6组必须自然完成，并从原输出验证自动模式完整13列正值或线程模式所有请求方法的
Children及Max per-process正值；只留marker、缺列/方法、重复或非有限值都失败，不信任reported_sections。7组必须进程正常结束并与固定Linux的版本不可用结论一致，
这项排除不代表向量I/O验收。启动拒绝、超时或不完整记录不能生成通过归档；旧C6九条消费者记录已被此门禁拒绝。
冻结的kernel/ELF/fixture/firmware/DTB和工具/源码身份在每次启动前写入input.json并封存哈希，执行器串行互斥。
`tests/cost-evidence.py` 压缩零指标后仍重建完整快照并核对seal，拒绝遗漏非零计数；
持久验收再次检查epoch严格递增和直方图最大值，后台取消和回收重入OOM由独立host回归保护。

即时fixture begin为当前执行上下文初始化run/idle起点，end先结算最后一段；
运行有效位与时间戳分开，时钟从0开始也不漏计。延期U-mode仍由用户返回边界启停。
全局cancelled使用实际任务归属；回收重入导致的合法EMPTY仍计失败，不改变重入禁令。

即时fixture还裁剪已开放IRQ区间的起点，并在end结算窗口内尾段；真实hart仍关闭时保留
开放状态，后续enable不能污染已完成窗口。全局cancelled的单位是丢弃的在途scope深度，
正常exit也可能贡献，不能解释成取消用户操作数；rank取消与原wait status分别报告。
C4按握手确认的N+4个blocked成员检查扫描max下限，遗漏扫描或只计有期限任务都会失败。

最终20配置、60启动、339窗口归档及14项报告检错通过；结论见[最终成本报告](../learning/cost-baseline.md)。
`cost-evidence.py --final --output` 接受用户 runner 列表与 io-sleep fixture 单记录，按真实配置收集三副本，
规范化不修改封存输入，并从原消费者输出重新核对完成分类；解包会验证完整固定矩阵和全部快照seal。
新增九次串行启动、48个窗口的原消费者续测见 `cost-consumer-followup.json`，由
`tests/iozone-closure.py --verify` 独立核对，18项报告检错通过。兼容分支保留uname 4.15.0，
旧glibc/musl各七组可用测试全部完成，原向量组在固定Linux同样不支持；这项排除不是向量ABI通过。
完整消费者观测开销中位16.964%，旧180秒预算不足，不能将历史取消序列与完整序列混算。
原judge与3600秒预算另在评测分支运行；其状态和分数不能由观测窗口的complete字段推导。
本模块的采样验收、实际消费者完成、全套评测通过是三个分别检查的条件。

原版消费者定点归因可用 `--consumer-commands musl:0,musl:1,glibc:0,glibc:1`；部分集合的验收范围仅为该集合，不能替代完整八组。逐命令程序时间和排空时间分开保存；窗口关闭前包装器同步当前工作目录中的普通文件与目录。事务内存指标的 max 是按堆容量和 home buffer 保守计费的峰值，value 是每批峰值样本之和，不能当瞬时驻留量。

设备请求保留观测标量的完成观察时刻，实际唤醒 owner 后记录同一请求从完成观察到 wake 返回、到原 submit 调用恢复的 ticks。`io_complete_to_ready` 是 wake 返回后的上界，`io_complete_to_resume` 包含 ready 等待与恢复后的少量记账；正常完成但设备返回错误的请求也可采样，错误另有计数，没有完成观察的 reset 路径不采样。batch 会合并唤醒，未实际 wake 的请求不在关联延迟子集中，不能与旧所有请求样本直接比较平均值。它们不保存任务引用；关闭构建无这些字段。S7 增加 checkpoint 耗时及三种等待原因后，固定聚合与声明预留为 65405 B，每任务仍为 64 B，均由快照输出和编译期断言核对。

定点消费者启动可追加 `--consumer-sync`：同一启动中使用当前静态 musl 协调器执行四组固定 4KiB 热覆盖写，各 128 次，分别 O_SYNC/O_DSYNC/每次 fsync/每次 fdatasync，另核对读回内容。`--consumer-sync-only --linux --replicas 1` 只执行这些受控同步参考，不能称为原版 iozone 或完整消费者验收。原版八组 argv 不受该选项影响；同步窗口独立命名，计入本次新增窗口范围。

`--coordinator-elf` 可直接复用先前封存的 consumer ELF，保证原消费者 ON/OFF 的协调二进制一致；执行器仍冻结它并记录实际哈希。该选项不与新增同步 probe 混用。短同步参考可另用 `--consumer-sync-only`；其窗口不计入原 iozone 成绩。

历史 v1 存档按原完整指标表解包：接受当前表及截止 wake_to_run 的原表，不能接受任意删减的表。旧记录无需补造 journal 指标或重跑测量，旧 seal 与九启动身份继续核对；新窗口仍要求当前完整表。

异步日志验收证据追加在既有 `cost-consumer-followup.json` 的 `journal_optimization`：三次原消费者 OFF、
一次四命令 ON、两次受控同步参考；不冒充三副本 ON 或完整成本矩阵。四个 ON snapshot 保存完整
v1 `values` 与 SHA256，仍可直接送入 `cost_report.parse` 核对 epoch、完成状态、单位和溢出；
历史根记录及 seal 保持原样，`iozone-closure.py --verify` 继续只核对历史九启动。
各次身份、原输出、比较值和开销限制见[验收分析](../learning/cost-baseline.md#异步日志与组提交验收2026-10-01)。
