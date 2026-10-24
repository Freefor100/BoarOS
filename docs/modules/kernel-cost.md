# 成本观测模块

`COST_DIAGNOSTICS=1` 在 `build/cost/riscv/` 构建观测版本，内核为
`build/cost/kernel-rv`；`SMP=1 USER_SMP=1 COST_DIAGNOSTICS=1` 的两核版本独立构建在
`build/cost-smp/`。默认关闭观测。C/assembly 同用 `BOAROS_COST_DIAGNOSTICS`，
每个对象依赖只在配置改变时更新的 `generated/cost-config.h`，包括显式覆盖
`BUILD_DIR` 的构建。关闭时宏为空，任务/VFS 节点布局无观测字段，proc 无观测节点。

`kernel/cost.c` 保持固定聚合，`kernel/sched/cost.c` 连接时钟、任务关系和用户返回。
诊断存储不持有任务、MM、inode 或 OFD 引用；任务仅有标量 epoch、运行/等待时间戳、
在途深度及控制抑制状态（标量48字节）；任务内两个 backend guard 和四个内嵌资源请求 guard 各新增16字节，
用户两核服务计时再加16字节，故任务字段总开销为单核144字节、用户两核160字节。
原任务元数据页容量断言继续生效。
热路径不分配、不打印、不睡眠。聚合由 COST 专用原子锁保护，同一 CPU 的递归深度先于
IRQ observer 回调建立，退出时恢复原 SIE。它不取得调度域、files 或 VFS 锁；任务关系
查询和 descriptor 判断在锁外完成。每 CPU 独立保存 trap/return 时间戳、IRQ 区间、
递归深度和实际执行者借用，共136字节；任务迁移只迁移任务计数，IRQ 区间仍属于原 CPU。
CPU 原有 service 计数另占40字节/CPU。用户两核注册表、固定聚合和这些 CPU 诊断字段
按 sizeof 合计84,964字节，编译上限96KiB，快照 `storage_bytes` 如实输出；
每任务字段另列 `task_bytes`，不计入固定聚合。物理 IRQ 入口在等待聚合锁前保存传入的边界；结束窗口先结算旧 return 尾段，
再裁剪当前关闭区间。聚合锁等待单列 observer，不重复累计同一物理 IRQ 区间。
任务增量仍为用户两核160字节；完整 CPU 对象按实际布局包含这些字段和对齐。
直方图保留全部65桶。

普通构建也不再为诊断单独读取设备服务时间、在途积分和发布阶段时钟；VirtIO 只保留
完成请求、字节、队列占用等运行统计，以及真实期限所需的时钟。详细时长由同一
`BOAROS_COST_DIAGNOSTICS` 开关启用。用户页解析复用 COST 计数，删除了每个解析
片段都更新的独立全局原子计数；关闭诊断后不保留这项探针。

仅观测版本提供 `/proc/boaros_cost_control`（完整 `begin\n`、`end\n`）及只读
`/proc/boaros_cost`。开始在发起控制命令的线程返回用户态时激活，结束由同一发起线程的返回完成；
其他 CPU 的用户返回不能代替它越过边界。结束在共享锁内检查真实在途数量，并结算
所有 CPU 的运行片段和已开放 IRQ 片段，再允许读取固定快照；控制 fd 的完整
read/write/readv/writev/定位读写在 dispatch 前识别并抑制自身计数。重复开始 EBUSY，
状态/命令错误 EINVAL，其他控制组结束 EPERM，在途结束 EBUSY，均不清零原窗口。
控制组退出中止窗口并标 incomplete。完整用户复制先于控制发布，坏指针 EFAULT。
控制节点使用同 OFD offset 串行机制，但完整命令不受当前 offset 限制。

consumer 和 write 包装器允许同步已达到 durable、后台 checkpoint 尚在途的窗口：被测操作
结束时先记录 `COST RESULT` 的时间边界；`end` 返回 EBUSY 时在用户态有界等待重试，
最多十秒，并另报 `COST CLOSING name ns retries`。关闭等待属于快照覆盖区间，
不能并入原操作延迟，也不能把原操作已成功解释成 checkpoint 已排空。双核 fixed metadata 同样复用这项有界关闭等待；其他受控
case 的严格结束检查不变；内核的 EBUSY/inflight/complete 契约不放宽。

`contract` 程序打印最后的快照与通过标记后，先 `fflush` 再 `tcdrain`，确认
TTY 输出队列排空才退出。仅清 libc 缓冲不足以阻止关机 raw 诊断插入 UART
队列中的快照行；runner 保持严格解析，不删除损坏行或把部分快照当成有效结果。

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
目前交付 contract/write/locking/mprotect/deadline/latency/consumer/readers/metadata/allocator/scheduler，
`all` 不跳过缺项；consumer 不将未完成命令标为完成。独立报告读器拒绝缺项、重复、未知键、单位错误、旧 epoch、
直方图不一致、incomplete 和 overflow。scheduler固定8192次yield、8192次带内容核对的管道往返，以及1024次创建/wait/让出执行；
三个业务窗口共wait完成1026个child；预热child在计时外。元数据资源归还另由生命周期门禁和整次启动基线核对。
各窗口时间必须为正；ON还要求真实IRQ和raw取得/持有样本。冻结ELF通过`--coordinator-elf`复用。
当前 Python discovery 不收集带连字符的文件，
因此 host target 直接运行 `python3 -B tests/test-cost-report.py`，必须实际执行测试。

fixture匹配测量可显式传`--qemu-seed`（uint64且整个副本跨度不得溢出），副本i使用
base+i，并在基线/候选/返回及ON/OFF间配对。默认不设置，official配置拒绝该选项。
它固定QEMU的客体PRNG，包括生成DTB及reset时更新的`rng-seed`，不把不可信启动材料
改成可信熵或伪造random ready，也不保证host crypto/外部RNG确定性。实际参数、种子
和DTB哈希进入启动前seal；resume同时核对身份和argv，不能混入未设置种子的旧启动。
固定依据为`references/qemu` commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`的
`qemu-options.hx`、`hw/riscv/virt.c`和`hw/riscv/boot.c`；固定timebase仍不代表固定速度。

`--fixture-cache PATH`只用于上述allocator/latency/locking/scheduler单盘BoarOS fixture。
首次在客体启动前封存完整镜像及case/ELF/长度/SHA身份，以整目录发布，随后各次启动
复制为独立可写盘。缓存损坏或半发布明确失败，不重新生成后冒充命中。三个副本的初始
内容相同，但实际root drive路径必须不同；fresh历史记录仍使用原校验规则。冻结模式及
内容身份进入seal，resume不能通过漏传或新增cache选项改变前置状态。默认和official不变。

C1 进一步按 file/stream/dgram/other 记录 calls、requested/accepted、实际 usercopy、页解析、
staging 累计请求容量、heap 请求字节和成功物理页。短写以实际接受前缀为准；同步尾部失败
不会抹掉先前接受量。全局 heap/page 指标是实际 allocator 入口和成功结果，按本层单位记录，
不能把 heap 字节、物理页及 staging 引用容量相加当驻留峰值。
缓存计 bucket probes、实际复制、范围写回候选访问、实际脏区间快照与逻辑后端接受量。旧两轮全页链与整页快照的历史值保持原解释，当前算法访问较小的候选集合。
设备按 registry 的磁盘0/1/其他聚合；请求在 submit 保存标量 epoch/lane，IRQ 完成和 reset
使用提交身份，旧请求不能污染新窗口。fixture 未登记设备属于 other；设备详细计时只在
观测构建产生，用户页解析由 `COST_USER_RESOLUTIONS` 统一记录。
提交路径按纯数据阶段、提交前正文、checkpoint metadata 与 journal superblock 设置标量 phase，设备按真实提交上下文归因。`device*_precommit_body` 包含同一阶段的 ordered data 与日志正文，不声称区分二者；commit 记录计入总写入。历史 `device*_journal_write` 由旧记录格式读取，不出现在当前诊断输出；其余字节继续归 unknown_read/unknown_write（含之后失败的请求）。请求字节不表示持久字节；后端逻辑数据不足以证明其他扇区分类。
复制/解析和设备请求由 scale 的独立 wrapper/原统计交叉验证；三个副本及完整窗口由
`tests/cost-summary.py` 再验证。C1 结果和开关开销见[成本基线](../learning/cost-baseline.md)。

`tests/cost-riscv.py --case metadata --metadata-fixed-only --metadata-parallel` 在同一个协调 ELF 中执行固定总工作量：双任务 stat/open 各总计4096次、rename总计2048次、追加总计16MiB，另保留既有目录负载。单客核时两个任务都在CPU0，双客核时分别固定CPU0/1；单任务在双核时固定CPU1。每个修改窗口在计时结束前完成对应文件或父目录的真实fsync，最后全局排空单列。

fixture runner 的 `--host-vcpu-cpus 0,2 --host-io-cpu 4` 通过QMP暂停启动，核对真实vCPU线程、设置并读回亲和性后恢复；主线程及已有I/O线程留在I/O核，后续块I/O worker沿QEMU主线程创建规则继承。`--qemu-seed` 写入匹配身份；同一比较组使用同一ELF、工作量、输入和完成条件，before/control内核逐字节一致。正式耗时使用关闭诊断的内核，观测版本单独解释锁与日志经过时间。原格式记录按整行、字段集合和唯一性解析，尾随垃圾、重复字段或损坏的额外记录直接报错。

QMP 的 cpu-index 标识虚拟硬件 CPU，BoarOS 的逻辑 CPU0 则是 OpenSBI 选出的 boot hart。需要比较 CPU0 协议等固定职责时，`pin_qemu_threads` 可同时接收 RV 内核 ELF 和 GDB socket：在物理 kernel_main 入口核对 mhartid、a0 和 boot_cpu，删除断点并断开调试器，确认客机保持暂停，再按逻辑 CPU 顺序绑定宿主核后续跑。该入口早于 secondary 和用户任务启动。GDB 或映射校验失败时，QMP 停止并确认暂停后传播错误，调用者负责终止 QEMU。未提供这两个参数的调用沿用物理 CPU 顺序。

C2 按 rank 10/15/20/21/30/31/32/35/40/other 记录尝试、取得、阻塞、唤醒、重阻塞、取消及等待/持有直方图。
资格等待包含 grant 前实际发生的诊断工作，按观察到的经过时间解释。
等待止于 metadata 资格中实际授予 writer/readers 的时刻，持有由该 grant 开始、
在实际撤销资格时结束；随后等待诊断发布不能改变这两个边界。排队者从授予预留起
计入占用，恢复运行延迟不再计入资格等待。释放后的定点 scope 提交仍消费真实在途登记。
持有 scope 参与在途计量，guard 只新增开始时刻和登记标量，不持有诊断任务引用。
独立假时钟测试区分前台运行20 ticks、后台运行890 ticks、前台睡眠880 ticks和就绪10 ticks。
低内存 io-sleep fixture 使用相同接口输出 pressure-io/timeout-cancel，完成后才分配快照；
名称/单位使用字符数组，避免 fixture 关闭分页后解引用高半区绝对字符串指针。
真实 U-mode 的13个 locking窗口检查同/独立OFD、同/不同inode、1/8/32等待者及双盘，
覆盖冷映射输入、向量追加、O_SYNC和截断。U-mode取消阶段在write前撤销gate等待者；
实际写入在途取消由低内存io-sleep的operation_mode=3保护。四种设备配置各三次 fixture 启动验证清理。

C3 计数覆盖外围VMA查询/覆盖/权限/数组编辑/合并/recount、resident遍历和实际PTE/TLB，
prepare/commit/整次改权为嵌套墙上耗时，不能相加。聚合上限包括名称、单位、索引、锁映射表和 CPU 本地状态。

C5 使用每 CPU 独立状态跟踪实际SIE转换，嵌套不重起，切换不截断；采样段及明确盲区见成本基线。
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

历史20配置、60启动、339窗口测量及14项报告检错通过；结论见[成本报告](../learning/cost-baseline.md)。
`cost-evidence.py --final --output` 接受用户 runner 列表与 io-sleep fixture 单记录，按真实配置收集三副本，
规范化不修改封存输入，并从原消费者输出重新核对完成分类；解包会验证完整固定矩阵和全部快照seal。
原消费者续测为九次串行启动、48个窗口，当时由`tests/iozone-closure.py --verify`
核对生成报告，18项报告检错通过。运行报告保存在`build/`，不纳入Git；报告解析
测试独立构造小型样本。兼容分支保留uname 4.15.0，
旧glibc/musl各七组可用测试全部完成，原向量组在固定Linux同样不支持；这项排除不是向量ABI通过。
完整消费者观测开销中位16.964%，旧180秒预算不足，不能将历史取消序列与完整序列混算。
原judge与3600秒预算另在评测分支运行；其状态和分数不能由观测窗口的complete字段推导。
本模块的采样验收、实际消费者完成、全套评测通过是三个分别检查的条件。

原版消费者定点归因可用 `--consumer-commands musl:0,musl:1,glibc:0,glibc:1`；部分集合的验收范围仅为该集合，不能替代完整八组。逐命令程序时间和排空时间分开保存；窗口关闭前包装器同步当前工作目录中的普通文件与目录。事务内存指标的 max 是按堆容量和 home buffer 保守计费的峰值，value 是每批峰值样本之和，不能当瞬时驻留量。

设备请求保留观测标量的完成观察时刻，实际唤醒 owner 后记录同一请求从完成观察到 wake 返回、到原 submit 调用恢复的 ticks。`io_complete_to_ready` 是 wake 返回后的上界，`io_complete_to_resume` 包含 ready 等待与恢复后的少量记账；正常完成但设备返回错误的请求也可采样，错误另有计数，没有完成观察的 reset 路径不采样。batch 会合并唤醒，未实际 wake 的请求不在关联延迟子集中，不能与旧所有请求样本直接比较平均值。它们不保存任务引用；关闭构建无这些字段。S7 增加 checkpoint 耗时及三种等待原因后，固定聚合与声明预留为 65405 B，每任务仍为 64 B，均由快照输出和编译期断言核对。

定点消费者启动可追加 `--consumer-sync`：同一启动中使用当前静态 musl 协调器执行四组固定 4KiB 热覆盖写，各 128 次，分别 O_SYNC/O_DSYNC/每次 fsync/每次 fdatasync，另核对读回内容。`--consumer-sync-only --linux --replicas 1` 只执行这些受控同步参考，不能称为原版 iozone 或完整消费者验收。原版八组 argv 不受该选项影响；同步窗口独立命名，计入本次新增窗口范围。

`--coordinator-elf` 可直接复用先前封存的 consumer ELF，保证原消费者 ON/OFF 的协调二进制一致；执行器仍冻结它并记录实际哈希。该选项不与新增同步 probe 混用。短同步参考可另用 `--consumer-sync-only`；其窗口不计入原 iozone 成绩。

历史 v1 存档按原完整指标表解包：接受当前表及截止 wake_to_run 的原表，不能接受任意删减的表。旧记录无需补造 journal 指标或重跑测量，旧 seal 与九启动身份继续核对；新窗口仍要求当前完整表。

异步日志验收范围为三次原消费者 OFF、一次四命令 ON、两次受控同步参考；
不冒充三副本 ON 或完整成本矩阵。生成的 ON snapshot 可送入`cost_report.parse`
核对epoch、完成状态、单位和溢出；`iozone-closure.py --verify`核对传入报告的范围。
比较值、机制解释和开销限制见[验收分析](../learning/cost-baseline.md#异步日志与组提交验收2026-10-01)。

缓存纠错观测另计块缓存hit/miss/eviction/load_wait；hit表示取得已有缓冲引用，
不保证内容已经有效，因此loading命中仍可等待。设备READ在实际发布处只按可证明的
file_read/metadata_read/prepare_read归因，不能证明的仍为unknown。来源scope嵌套恢复，
不改变既有OUT的data/journal/checkpoint/super分类。名称容量32字节，逐项编译断言；
这13项无直方图指标共用本页开头的固定预算；历史182项schema继续可读。
`--case readers`是统一起跑、每任务8MiB/1KiB的同/不同inode冷/热读诊断，
全部工作者完成且内容/状态核对后结束，记录标量时间和字节，不保留对象引用。

`--consumer-readers`在所选原消费者之后追加四个固定工作量窗口。同一协调ELF可用
`--coordinator-elf`复用；consumer/readers镜像均预置五个8MiB文件，改变了原S9前置状态，
因此只与相同前置文件、命令集合和协调ELF的旧版对照比较。独立readers记录逐任务
统一放行、实际开始/完成时间、字节、校验值及成功退出；原程序停止规则另由固定源码解释。

`ROOT_DRAIN_FIXTURE=1`仅用于独立构建目录，通过链接wrap测量内核卸载调用，默认构建
无此钩子。boot先卸载子挂载、最后根挂载；保留全部成功记录，最后一项报告根盘剩余
checkpoint与卸载清理时间。它不是全程checkpoint总耗时，也不包括调用前已停止worker的
时间，不增加用户syscall。`--kernel-identity`核对外部构建二进制与源码身份；
`--trace-read-lbas`保存QEMU实际读范围，范围是整个启动（含初始化/格式化读取），不能
冒充窗口内重复读。`--resume-from`仅重解析身份匹配的完整旧日志，继续未执行副本，
保留原输入seal；不会重新执行已完成程序来修正解析问题。

缓存纠错匹配旧/新各3个OFF、各1个定点ON及各1个固定读OFF，共10启动、16快照。
执行器的metric_schema及seal明确区分旧182/新195项；新schema热路径观测会明显
扰动固定热读，成绩采用OFF。旧版外部构建的
JSON指标表先规范化为受支持注册表，再做完整字段/epoch/溢出验证；不接受任意删项。
程序、应用durable收尾、剩余内部卸载各自报告；有限收益和未唯一归因的历史分布保留，
不把这10启动扩写为原八组全套或完整成本矩阵。

`journal_prepare_ticks` 从外层操作成功 attach 计到接受前 detach，覆盖实际私有准备、
解析及等待；嵌套操作不重复登记，失败准备也结算。每个活动操作在诊断构建中保存40字节
scope，OFF 无该字段。`journal_record_prepare_ticks` 只计每次实际记录写视图的准备；
`journal_publish_ticks` 计校验、合并和发布。三者是嵌套经过时间，不能相加当 CPU 时间。
`journal_resource_retries` 计完整回滚后真实消费排序重放请求的次数；
`journal_memory_retries` / `journal_log_retries` 只计调用者明确遇到配额/日志预约失败、
完成回滚和回收等待后重新尝试的次数。真实 malloc 失败和磁盘 ENOSPC 不计作可回收重试。

## 日志批次定点归因（2026-10-02）

新增五项成功封口原因：operations、bytes、age、target、force。条件可以重叠，
只能与组数对照，不能相加；operations仅用于旧策略对照，取消操作数阈值后为零。
新增sealed/durable/checkpoint等待ticks，记录累计值、样本数与最大值；计时覆盖
释放后端锁、阻塞到恢复取得锁的经过时间，不能当成CPU耗时或跨任务可加的总时间。
旧195项registry仍明确可读，schema版本1不变；当前存储预算和任务开销见本页开头。
关闭COST时没有这些诊断状态或采样代码。`make test-cost-host`保护接口、预算和解析。

## 内存消费者的有限计时（2026-10-03）

新增六项无直方图指标：heap_zero_bytes/ticks、heap_copy_bytes/ticks、
uaccess_copy_bytes/ticks。它们只包围实际 calloc 清零、resize 搬迁和成功解析后的
固定长度页内复制；公共 memcpy/memset 不采样，避免观测初始化递归。bytes 的值为
实际累计字节，samples 是执行次数，max 是单次最大字节；对应 ticks 保存每次经过时间。
采样前捕获 epoch/前后台身份，睡眠或切换后仍按该来源记账；不把经过时间称为独占
CPU 时间。零字节成功执行可以有样本，失败准备不能伪造工作量。

这些指标与其余注册表共用本页开头的固定预算；编译与快照均检查容量。追加前的
完整203项 schema 继续可读，当时的209项必须齐全，版本1不变。关闭观测时没有新状态
或指令。独立消费者 fixture 核对真实范围、失败边界、样本和时钟，而不是只测试
公共内存函数。Lua 的一次定点窗口及候选未上线结论见
[工具链记录](../learning/offline-toolchain-probe.md#内存操作的有限归因与未上线候选2026-10-03)。

测量协调器的stdout经过TTY队列：`fflush`只清空libc缓冲，不能证明UART已发送完。
输出最后一份快照后用`tcdrain`等待实际排空，再退出，避免关机raw日志插入快照尾部。
损坏快照仍按严格格式拒绝；既有完整业务窗口可单独核验并注明观测范围，不能修补原始
输出或把缺失的末窗口写成通过。

增长指标 `resize_visits` / `resize_tail_pages` / `resize_alias_rearms` 区分缓存项访问、
实际处理尾页及别名重新保护；哈希探测沿用 `cache_probes`。新指标追加在注册表尾部，
解析器继续接受固定的旧完整 schema，缺字段不能当零。指标名按实际长度保存，
聚合存储受本页开头的固定上限保护；测试约束这一公开预算，不绑定某个私有布局字节数。

网络服务指标在版本 1 末尾追加，旧 schema 仍由报告器识别：服务批次、socket 工作单元、loopback 包、timer 回调、全局 registry 扫描、poll 查询及查询触发服务、带可执行工作睡眠次数。socket 工作单元含容量转交与到期重试，不能解释为唯一 socket 数；末项为睡前诊断谓词，正常应为零。没有新增对象引用或动态观测内存，聚合受同一固定上限保护。

TCP admission 新增 `stream_admit_blocked`（预先无容量的尝试）、`stream_protocol_blocked`
（复制后提交仍为 EAGAIN 的尝试）和 `stream_copy_blocked_bytes`（尚无进展时这些失败新复制的
payload 字节）。阻塞调用可能有多次尝试，不能当作 syscall 次数；复用暂存后缀不再次计复制。
现有 `stream_copy` 仍记录全部真实复制。指标在末尾追加，前一网络 schema 继续可读。


## buddy元数据观测（2026-10-09）

注册表末尾增加page_meta_checked、page_meta_written（逻辑记录）和allocator_meta_ticks
（区间累计ticks/samples/max），均不带直方图。每次元数据区内局部累计，离开后一次
原子提交三个计数；默认关闭，不持对象引用、不分配观测内存。计时不含压力回收与I/O
等待。raw区间观测共用同一固定预算，旧223/226项完整registry仍可显式解析；缺少新
指标标记不可用，不能填零，也不接受随意缺字段的快照。外部冻结kernel identity中的
registry与二进制SHA必须匹配；同时声明两份registry时必须一致。

`python3 -B tests/cost-riscv.py --case allocator`执行1页×1024、64页×64、4096页×4
的固定匿名映射、逐页零值/内容检查和撤映射。allocator及既有latency均支持
`--coordinator-elf`冻结同一个ELF，`--kernel-identity`冻结构建身份，默认三次独立启动。
allocator复用既有10秒异步关闭协议：实际工作计时先停止，checkpoint在途scope
排空所需的关闭ns/retries另记；EBUSY不能被吞掉，超时继续失败。最终输出用真实
fflush/tcdrain排空TTY，原始快照损坏仍拒绝。启动预算保持180秒。

匹配旧/新/返回旧、ON/OFF共36次启动及负向结论见
[学习记录](../learning/cost-baseline.md#buddy-森林匹配时间2026-10-09)。IRQ-off直方图与
rank持锁/wake-to-run沿用RV观测；LA本轮仅功能和16KiB工作量门禁，没有声明LA时间验收。

raw_wait_ticks记录轮询/原子获取区间（无争用获取也有样本），raw_hold_ticks记录取得后
到释放前的区间；二者只记录累计ticks/samples/max，不增加直方图。默认关闭，guard仅
存栈上tag与时间，不固定新owner；释放raw后集中提交。元数据计时从取得锁后开始，
排除锁获取与压力/I/O。heap→page嵌套区间可能重叠，不能把raw_hold与meta时间相加当独占CPU时间。
完整旧226项registry缺少这两项时标为不可用；任意缺字段或部分新增schema仍拒绝。

## 短窗口匹配与宿主策略租约

`make test-cost-match-host` 验证配对统计、全轮测量锁、CPU策略恢复和完整日志门禁，
也是 `test-cost-host` 的依赖。`tests/cost-match-riscv.py` 使用固定的
`build/data-path-baselines/sync-b3/` 输入；开始前校验原B3基线、当前OFF核的源码差异与
加载段、工具、原ELF及只读预启动盘。没有缓存时明确失败，不重新准备盘后冒充相同输入。

协议 `b3-short-v1` 每个case固定12个三启动配对块，三个既有seed各使用四次。
校准为同一当前核的三次启动，比较为原基线/当前核/同一原基线。完整块重采样10000次，
每次重新计算三相中位数及首尾基线均值；不得独立重采样列或平均块百分比。
校准95%双侧区间须在±1%内，首尾漂移须在±2%内；比较单侧95%上界不超过3%才通过，
下界超过3%为稳定超标，其余未决。区间是有限样本筛查，不能排除系统性偏差或证明普遍等价。
所有原窗口都保留，各自校准未决时不记置信通过。新增95%门槛只用于两个目标；
非目标继续保护原点估计3%及漂移2%门槛，不扩大验收范围。部分case结果不能声明B3整体完成。

driver与普通用户恢复监护进程持有同一全局 `measurement.lock`，控制进程死亡或超时后，
监护仍保留真实锁描述符直到负责恢复的helper退出；单次runner通过已验证的
继承描述符复用锁。单次记录仍是 `replicas=1, acceptance=false`；完整验收属于外层manifest，
不改变原三副本接口。完整schedule在第一次启动前封存，校准失败的case不进入比较，
不追加样本、筛掉慢副本或重用缺少真实终态的旧日志。

CPU策略helper只操作CPU2/3的governor、频率上下限及governor改变的EPP，请求performance/
2500000kHz；包含其他CPU的共享policy先拒绝。测量与文件写入仍由普通用户执行，helper
不接受工作命令。正常结束、配置失败、SIGINT/SIGTERM和控制管道EOF都进入恢复，逐项
核对原值；cpufreq上下限提交后异步生效，每项只写一次，最多等1秒确认读回再写下一项，
不把请求被接纳当作恢复完成。恢复失败仍发送原值、实际值及具体错误；恢复未验证即失败。
策略读回、频率观察、同胞活动与节流计数各自记录，
缺失标不可用；固定请求频率不证明实际频率恒定。

runner封存完整input/result/boot.log及实际退出状态；离线校验重新解析所有工作量、
locking的168条native wait status和唯一完整PID1终态，核对复制的kernel/firmware/ELF/DTB。
root成功行只在冻结源码的 `root_boot_finish` 完成worker/挂载/设备销毁、heap及page基线检查后
输出；这是既有root门禁执行证据，不是独立打印了所有等待和设备owner的零计数。
输出目录必须新建或为空；当前协议不resume或拼接旧启动。

```sh
make test-cost-match-host
# 只验证真实策略取得/恢复，不启动客户机；末条 restored 的 errors 须为空。
printf '{"command":"restore"}\n' | sudo /usr/bin/python3 -I tests/cost_policy.py --lease
# 在允许sudo取得root的普通宿主终端预先认证，仅helper取得权限。
sudo -v
make test-cost-match-riscv COST_MATCH_OUTPUT=build/cost-match-new
python3 -B tests/cost-match-riscv.py --verify build/cost-match-new
```

权限不足在任何正式启动前记录 `environment_blocked`，不回退到powersave测量。
本机普通终端的当前清页核及隔离FS-Off候选各完成72次同核校准，两个目标均未通过精度门槛，
各自比较阶段全部跳过，候选没有成本验收通过资格；
更早执行器的权限阻塞单列保留。当前成本状态及历史结果见
[共享调度域匹配时间](../learning/cost-baseline.md#共享调度域的匹配时间)。

### 只读执行器归因

`tests/cost-diagnostic-riscv.py` 提供单独的 instrumented profile，复用单次启动、固定盘和完整
工作量/退出检查。准备时封存原 ELF、候选核加载段、执行器、插件、符号 PC 和运行源码；
wrapper 另记录实际含插件的 QEMU argv，与单次 runner 记录的 wrapper 身份分开核对。
尝试槽位和 exec 收据均独占创建；失败、超时或中断不补跑，不 resume。此 profile 的
`acceptance` 和 `performance_verified` 始终为 false，不进入正式匹配成绩。

观测边界为原 ELF 的 `clock_gettime` 入口，和客体取时点有偏差；同时记录 wall、vCPU线程
CPU时间及自愿/非自愿切换次数。wall−CPU包含WFI、I/O及宿主调度，切换次数不能换算成
离核时长。两个时钟并非同时取样：每次CPU/切换计数取样都有MONOTONIC前后包络，
报告wall−CPU的有符号差及包络误差区间，不把包络内的负差值判成无效。
TB长度×执行次数只是工作量代理：异常可能提前离开TB。FS取首次执行的样本，
仅为翻译键的一部分；hardware-address值仅在同次启动内作不透明身份比较。全局code-cache
flush与客体TLB失效分开。所选函数入口事件不代表完成，符号体内TB代理不包含其callee；
共享helper的业务owner没有独立证据时保持未知。热回调不输出或分配，固定容量溢出使诊断无效。

```sh
make test-cost-diagnostic-host
# 本地观察器额外依赖 QEMU plugin API 头文件和 GLib 开发文件。
make test-cost-diagnostic-plugin-host
python3 -B tests/cost-diagnostic-riscv.py --prepare build/residual-diagnostic-new
sudo -v
python3 -B tests/cost-diagnostic-riscv.py --run build/residual-diagnostic-new
python3 -B tests/cost-diagnostic-riscv.py --verify build/residual-diagnostic-new
```

固定资料：`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`kernel/sys.c`定义RUSAGE_THREAD；`references/qemu@84f07211cc5b4fc6a371559bf8a5de4fb068e648`
的`target/riscv/cpu.h`与`tcg/tcg-cpu.c`定义FS翻译键。实际插件编译头、QEMU二进制及工具SHA
由manifest另行封存，参考源码版本不替代执行器身份。
固定FS-Off核的12次诊断已通过108个原窗口及完整自然退出核对，策略恢复无错误。
已确认后台journal阶段与等待变化，剩余定量边界见[当前成本结论](../learning/cost-baseline.md#共享调度域的匹配时间)；
该结果没有性能通过资格，不替代正式校准。
短窗口归因已停止，保留方法与证据供实际需求复用，不再作为RV两核规划的前置门槛。
