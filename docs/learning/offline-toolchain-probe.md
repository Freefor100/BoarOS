# 客体内编译：从小探针到原版工程

宿主交叉编译生成RV64 ELF，只证明宿主工具链和客体执行路径。客体内编译需要把
原生RV64编译器、汇编器、链接器、libc开发文件及依赖安装到同一离线磁盘，实际执行
构建规则，再运行新产物。目标文件存在、版本字符串正确或包装器退出0，都不足以
证明这条链路成立。

固定输入由references/sources.tsv和准备脚本校验。当前为Alpine v3.22的
GCC14.2.0-r6、binutils2.44-r3、musl开发环境和GNU make4.4.1-r3；make许可
GPL-3.0-or-later，Lua5.4.3许可MIT。工具和Lua保持原包/发布内容。
固定Linux依据位于references/linux（v7.2）；精确输入身份归机器清单，人类记录
保留版本、机制、结果与重建入口。

## 小探针怎样发现通用缺口

2026-09-27先建立没有编译器的诊断镜像，Linux和BoarOS均在预处理阶段无法exec，
后续四阶段跳过。这是有效的失败分类，不能称为离线编译成功。

安装原GCC后，cc1以O_NOCTTY打开源文件，原内核的flag校验返回EINVAL；接纳该
通用flag后，又在GCC对链接产物执行fchmodat时遇到ENOSYS，最终exec返回EACCES。
修复使用lwext4的活inode句柄与事务，保护unlink后的fd、ctime及错误owner。
最终预处理、编译、汇编、链接、运行五阶段在两侧均退出0，产物逐字节一致。
2026-09-28的VFS/proc整合后复验保持一致。

默认的小型探针仍由`make test-offline-c-riscv`运行；无工具诊断入口为
`make test-offline-c-baseline-riscv`，两者判定边界不同。新项目选择不会改变默认行为。

## 镜像与参考环境也必须有效

固定Linux的PID1退出以panic停止客体，并没有正常卸载ext4。fsync后的目录项可能
仍位于journal中，直接debugfs查询会误报缺失。执行器先重放日志，再检查一致性；
只有明确属于异常退出的orphan清理才允许一次限定修复，其他fsck修复均失败。
依据为references/linux/fs/ext4/orphan.c。BoarOS则在可信启动栈回收任务和根挂载。

[CI run36](https://github.com/Freefor100/BoarOS/actions/runs/36513240987)中SQLite
通过而离线GCC在根盘第一请求超时。该现场没有请求类型，不能归因于ext4或SQLite。
原复制会把稀疏镜像扩成大量零写；执行器改为保留空洞并在启动前同步副本，镜像
内容和内核一秒超时保持不变。

[CI run37](https://github.com/Freefor100/BoarOS/actions/runs/36515194047)中的新失败
是固定Linux在旧QEMU上SIGILL。相同版本复现和反汇编定位到用户浮点指令；旧DTB
只提供riscv,isa，精简Linux配置未启用ISA回退，未开启用户浮点状态。
启用固定Linux的CONFIG_RISCV_ISA_FALLBACK后，本地QEMU8.2.2与11.1.1均通过。
依据为references/linux/arch/riscv/kernel/cpufeature.c和Kconfig。此结论是本地复现，
不替代新提交在远端CI上的结果。

## 管道与默认FIFO jobserver（2026-10-03）

匿名pipe原先固定返回0600，fchmod返回ENOTSUP。mode、创建时间、ctime和稳定身份
现归共享pipe；两端、dup/fork/proc重开一致，改权保留FIFO类型和原访问方向。
匿名pipe读写不更新文件时间。坏stat指针使用raw syscall，避免把libc转换时的用户
fault误当作内核errno。

命名FIFO的身份、权限与时间归真实ext4/tmpfs inode，传输与等待归pipe。
节点只保存弱关联，每个打开中和已打开的OFD另持节点与pipe。最后owner先摘关联、
丢弃缓冲，再关闭VFS引用，避免循环引用和重复端点归还。硬链接与改名沿用实例，
unlink后已打开实例仍有效，同名重建则隔离为新inode。

固定Linux的fs/pipe.c以到达计数保护“对端出现后立即关闭”。只重查当前人数会漏掉
会合并再次睡眠。BoarOS采用到达代次；初始非阻塞只读尚未见过写者时抑制HUP，
随后写者离开才报告HUP。信号重启、取消和fd满均不残留资格。对象与64KiB连续缓冲
分配失败后可重试，最终归还owner。只读挂载允许FIFO传输，无法取得挂载写资格时
跳过时间更新；节点可以同步持久化，传输内容永不恢复。

同ELF先证伪旧mkfifo行为，再验证两种文件系统、打开会合、poll/select/epoll、时间、
信号、只读挂载和重启。重建`make test-fifo-riscv`；OOM入口为`make test-files-riscv`。
语义依据为references/linux/fs/{pipe,open,inode}.c。

## 原版Lua工程的完整流程（2026-10-03）

使用34个C源文件与原Makefile，在客体运行`make -j1 linux`和`make -j2 linux`。
两侧完成干净构建、无变化重建、仅lapi.c改变的增量、clean后重建、明确语法错误的
非零退出及恢复。对象与lua/luac/liblua.a的内容保持一致；无变化重建不重新编译，
增量只更新必要对象、归档和链接目标。新Lua实际执行文件/模块和受控子进程，luac
完成字节码往返，静态库嵌入程序与C动态模块运行。同步后重启仍可使用这些产物。

默认make jobserver确实使用命名FIFO。递归负载的共享计数证明峰值两份资格，六个
任务全部完成且令牌归还；中断后子进程被回收，GMfifo节点被删除。正式性能输入
不使用这套资格诊断包装器。

参考流程曾因镜像缺/bin/echo失败：make可直接exec简单recipe，因此shell中能执行
某工具并不代表PATH里有对应程序。补齐工具链接后Linux的窄探针和完整流程成立。
错误恢复驱动还使用过未实现的路径truncate系统调用；现用已有ftruncate恢复输入。
这未修改Lua或make，路径truncate仍是能力缺口，不被这次工程通过掩盖。

重建`make prepare-offline-c-toolchain test-offline-project-riscv`。现有小C探针默认
不变；项目执行器记录每条命令的argv、cwd、真实wait status、耗时和产物身份。
运行目录与机器记录在忽略的build，Git保存本记录与可重建代码。

收口审查补到了信号与会合的交错：信号已将打开者唤醒，但它恢复前对端已出现又关闭。
固定Linux仍认领成功会合；旧代码错误返回EINTR，SA_RESTART还可能错过对端再等待。
以不同实时优先级控制单hart交错，先证伪再修复，读写两方向和两类信号均通过。
窄探针使用raw sched_setscheduler，因为固定musl的POSIX同名封装返回ENOSYS，不能
把该libc行为误报为系统调用缺失。

执行器的输入身份也改为在启动前固定：驱动取实际fixture中的init，Linux与BoarOS
均使用独立kernel快照；源commit、源树与未提交patch分开记录。运行结束不再重新
散列可被重建替换的输入路径。确定性测试在模拟启动中替换这些文件，保护这个契约。

## 构建成本与下一项依据（2026-10-03）

正式ext4测量使用modern/writeback、768MiB、单hart、宿主ext4、QEMU11.1.1，
两侧各三个串行独立启动。每个启动先干净-j1，随后clean并-j2；因此-j2的工具缓存
已经预热，不能将两列差值全部归因于并行。Lua、luac和静态库在六个启动中逐字节一致。
默认关闭观测，不设吞吐倍数或评分门槛。

| 命令/边界 | 固定Linux三个副本（秒） | BoarOS三个副本（秒） |
|---|---|---|
| 干净-j1 | 106.510 / 107.723 / 107.558 | 135.542 / 135.644 / 135.300 |
| 干净-j2 | 117.009 / 120.131 / 118.295 | 133.700 / 133.009 / 133.225 |
| clean | 0.032 / 0.035 / 0.035 | 2.513 / 2.548 / 2.503 |
| 最后显式sync | 0.036 / 0.035 / 0.035 | 0.009 / 0.010 / 0.008 |
| fixture最终根卸载 | 未提供相同边界 | 0.576 / 0.589 / 0.573 |

-j1中位数分别为107.558、135.542秒，-j2为118.295、133.225秒。
产物验证还包含嵌入程序和共享模块的客体编译，六次调用的中位为0.977、3.457秒，
不混入make构建时间。BoarOS完整启动的内核堆页面峰值约3.27–3.29MiB；这不包含
全部用户页或页缓存，不能写成整个系统内存峰值。每次最终heap-live为0。

程序完成、应用要求的同步、完整根卸载是三个边界。Linux PID1以panic停机，没有
测到与BoarOS相同的根卸载时间；不能把表中的sync35ms与BoarOS9ms当作同步性能
胜负。根卸载使用已有ROOT_DRAIN_FIXTURE，最后记录对应根挂载，早先记录可能为
已摘除的子挂载。没有新增用户系统调用。

一次tmpfs工作目录对照为Linux107.296/117.962秒，BoarOS133.971/132.374秒（-j1/-j2）。
BoarOS clean却降到0.061秒，Linux为0.030秒。这个对照说明工作目录的存储开销
不是整个构建差距的充分解释；它没有把编译器和共享库搬到tmpfs，因此不能据此
宣称所有I/O已排除。tmpfs输入复制另花0.051/0.409秒，未计入构建。它是单次归因
对照，不是另一个正式分布。

clean的代码链是make→rm→unlinkat→ext4_backend_unlink→ext4_orphan_free。
lwext4的ext4_reclaim_orphan在回收前、每个有界截断步骤后调用group_drain，保护
持久unlink/shrink intent及块/inode复用。这解释了为什么删除临时产物仍可能同步
推进日志和checkpoint；本轮没有删除这些屏障或修改回收协议。

一次定点COST只覆盖干净-j1、产物验证和在途同步。窗口170.493秒，前台/后台任务
运行合计156.791秒，idle11.691秒，记账余量约2.012秒。运行记账包含用户态、内核
和嵌套观测，不能把它写成“内核CPU耗时”。观测自身计时5.017秒也不能再次相加。
-j1在观测下为165.534秒，比关闭观测中位高约22.13%；该窗口用于归因，不能替代
正式成绩。多任务blocked累计845.921秒包含父等子等重叠，尤其不能当作单程序磁盘等待。

| 成本证据 | 值与解释 |
|---|---|
| 系统操作 | 384,836次；属于此工程窗口 |
| 堆请求 | 269,162次、累计413.7MB；不是峰值或全部无效分配 |
| 物理页分配 | 614,431次、接受629,795页；累计周转，不是同时占用 |
| 用户页解析/VMA比较次数 | 123,672 / 9,479,969；后者不是查询调用数，两项不能直接换算成时间 |
| 块缓存 | 命中267,630、未命中60,300、淘汰58,948；生产目标仍为8块 |
| 设备请求/FLUSH | 68,264 / 2,692；请求占比不等于耗时占比 |
| 读来源 | 文件30.2MB、元数据66.7MB、unknown150.1MB；unknown未强行归给ELF或重复读 |
| 日志组/镜像峰值 | 673组，单次实际峰值425,216字节；各组峰值之和不是内存峰值 |
| checkpoint等待 | 前台累计3.701秒；与其他时间存在重叠 |
| rank40 | 前后台累计获取等待1.079秒，重阻塞0；不能再称唤醒风暴 |
| mprotect | 659次，累计0.241秒；本工程不支持把范围改权列为主要耗时 |

观测聚合64,794字节、每任务64字节，epoch完整、无溢出、无在途操作，仍在原预算内。
当前lib/string.c的memset/memcpy是字节循环，实际RV64 memset反汇编为sb/addi/bne。
大量页周转使初始化/复制成为当时值得核对的CPU候选，但页周转次数不等于逐字节
清零工作量。匿名页、页表初始化和COW原本已有宽字路径；通用内存函数、部分heap
内部循环与固定长度usercopy仍为字节路径。本轮按下面的有限归因继续核对，不能由
分配次数把它称为唯一根因，也不据此更换调度器或扩大日志改造。

正式分布在完整工程候选上冻结采样；随后修正信号会合边界和清单记录。正常构建
没有该信号/短暂对端交错，因此复用这些样本，明确保留镜像版本边界，不重复无关
构建。旧清单已用保留的kernel/fixture/init快照核对修复，原清单保留到清理；未来
执行器在第一次启动前固定输入，避免运行中重建影响归因。

重建正式分布：准备工具链后，先以ROOT_DRAIN_FIXTURE=1构建独立fixture内核，再运行：

```sh
make prepare-offline-c-toolchain build/riscv/tests/user/offline-project-rv
make -j4 ROOT_DRAIN_FIXTURE=1 BUILD_DIR=build/offline-project/riscv-drain \
  KERNEL_RV=build/offline-project/kernel-rv all
python3 -B tests/offline-c-riscv.py --project lua --performance --repeat 3 \
  --kernel build/offline-project/kernel-rv \
  --program build/riscv/tests/user/offline-project-rv \
  --toolchain-tree build/offline-c/alpine-tree --timeout 900
make test-offline-project-tmpfs-riscv
```

COST构建使用COST_DIAGNOSTICS=1、ROOT_DRAIN_FIXTURE=1；相同执行器加
`--performance --observe --only boaros`，只测一个-j1窗口。工具输入与逐命令wait status
归本地机器记录。完整RV64、真实musl/glibc、1207条差分、scale、四组合io-sleep和
栈检查通过；信号会合修正后仅重跑受影响的FIFO/文件/信号与差分。未改事务、写回或
队列顺序，未重跑无关iozone或完整故障恢复矩阵。历史三项异常仍按goals保留。

最终候选已单向合入oscomp-rv-compat，原旧glibc BASIC_TCP完成wait=0、无超时，
后代清理为0；兼容配置的FIFO与重启也通过。主分支保留BoarOS uname，不将该旧glibc
结果写成默认main全面兼容。回到main后重建正常内核；本轮未push。

## 内存操作的有限归因与未上线候选（2026-10-03）

这次先回答“页周转是否意味着大量逐字节清零拖慢构建”，再决定是否替换内存原语。
六个默认关闭的指标只测 heap 的实际清零/搬迁和已解析用户页内 chunk，不覆盖匿名页、
页表、COW、字符串扫描、用户程序自己的 libc 或所有公共内存函数消费者。
VMA 原指标是比较次数，不能把约948万比较写成约948万次查询。

原 GCC/make/Lua 协调 ELF 不变。优化前一次定点 -j1 窗口为170.062秒，构建165.044秒；
COST 开销使它不能代替正式成绩。timebase为10MHz，以下为前台实际工作：

| 消费者 | 实际累计字节 | 次数 | 单次最大字节 | 内层经过秒 |
|---|---:|---:|---:|---:|
| heap 清零 | 33,555,658 | 188,027 | 131,072 | 0.057589 |
| heap 搬迁 | 399,616 | 762 | 4,096 | 0.000844 |
| 固定长度用户页内复制 | 54,948,562 | 111,561 | 4,080 | 0.111547 |

三段计时合计约0.170秒，仅为该窗口约0.10%；它们是内层经过时间，可能含中断，
不能称作独占CPU百分比。这足以否定“这些选定字节循环已经解释了整个构建差距”，
但没有排除页解析、缺页、ELF准备、未测消费者或用户态编译器成本。

实验分支 `codex/memory-primitives` 的候选采用 alias-safe 64位访问：对齐头尾使用字节，
memcpy 只在源/目标同余对齐时使用 word，memmove 按重叠方向处理；heap实际范围和
已解析页内chunk接入候选，不改匿名页、页表、COW或VMA。独立字节 oracle、全部
word offset、重叠、redzone、保护页、OOM/fault前缀，以及真实RV64 heap/uaccess通过。
一个关闭观测的微fixture在同一启动内比较冻结旧字节实现与候选，每点21批，真实timer
与中断保持启用。对齐4096字节copy中位从1192降至157 ticks，131072字节zero从1087
降至152 ticks；32768字节不同余copy为9264/9277 ticks，符合字节回退。空调用保留为
对照，不能把单点微实验速度当作整个工程收益。普通QEMU的rdinstret是宿主计数代理，
本次没有把它误称为客体指令数，也没有追加icount启动。

CPU候选在TTY合入之前冻结，modern/writeback、768MiB、单hart、宿主ext4，关闭
COST做三个串行独立启动。正式协调器没有加rusage或工具包装器，所有产物、错误退出
和资源清理成立：

| 阶段 | 三次秒数 | 中位秒数 | 原有匹配流程历史中位秒数 |
|---|---|---:|---:|
| 干净-j1 | 137.000 / 138.791 / 135.470 | 137.000 | 135.542 |
| clean | 2.449 / 2.506 / 2.469 | 2.469 | 2.513 |
| 干净-j2 | 136.564 / 135.321 / 133.357 | 135.321 | 133.225 |
| 显式全局sync | 0.008030 / 0.007667 / 0.008502 | 0.008030 | 0.009 |
| 最后根卸载fixture | 0.529901 / 0.510006 / 0.526844 | 0.526844 | 0.576 |

历史列来自上节原工程分布，相同协调ELF、工具、程序和启动顺序；旧运行目录已经清理，
不宣称恢复了旧镜像全部文件系统内部身份。三次新启动中的Lua、luac和静态库一致；
每次-j1/-j2都验证解释执行、字节码、静态库/C扩展及明确错误退出。根卸载是退出后
剩余工作，不是整个checkpoint总耗时；最终 heap-live=0，峰值heap占页836–845页，
累计分配约53.7万次，不能把累计调用当作峰值。

-j1/-j2相对历史中位分别增加约1.08%/1.57%，没有可验证的工程收益；这组小变化也
不足以独立证明某个内存原语产生了稳定回退。**宽字候选没有启用到main。**生产继续
使用原字节原语，保留六项有限诊断和实验分支；没有为得到正收益临时扩大重构或加跑
其他负载。下一项优化应从真实应用的未解释成本选择，不能继续把页周转等同于清零。

重建归因沿用上节工具链与ROOT_DRAIN_FIXTURE构建，加COST_DIAGNOSTICS=1和
`--performance --observe --only boaros`，只运行一次-j1窗口。实验分支提供
`make test-memory-host test-memory-riscv`；正式三个启动沿用上节
`--performance --repeat 3 --only boaros`。外部冻结内核可用`--kernel-identity`
核对二进制与原源码/config身份，与协调器当前来源分别保存。精确校验和、逐命令wait
status及原始输出只留在忽略的运行目录；这里保留版本、方法、绝对值和结论。
