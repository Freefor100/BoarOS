# 记录锁 owner 与 SQLite 回滚日志恢复

## 触发条件和契约

SQLite 3.53.4 原生 Unix VFS 在 `journal_mode=DELETE`、`locking_mode=NORMAL` 下使用 byte-range 记录锁协调独立进程；将锁只挂在 fd 或只挂在进程都会在 dup、fork、任意相关 fd close 和多进程 writer 竞争时改变可观察语义。固定依据是本地 `references/linux/fs/locks.c`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。Linux 的传统锁按共享文件表 owner，OFD 锁按打开文件对象 owner；两类锁之间仍检查读写冲突。固定 Linux 差分 profile 必须显式启用 `CONFIG_FILE_LOCKING`，否则参考侧也没有目标能力。

BoarOS 的每个活 inode 保留增广 AVL 区间树，owner 保存定向释放索引。锁节点不持 owner 强引用，避免 inode、OFD、文件表之间形成引用环。阻塞请求的 OFD pin 使请求范围在睡眠期间仍有效；传统锁在同表关闭该 inode 任意 fd 时释放，OFD 锁直到最后真实引用消失时释放。修改前预留所需节点使 `ENOLCK` 保持旧集合；close 和解锁不用新分配。单 hart 的关中断临界区覆盖冲突检查、登记和唤醒；未来 SMP 必须另建跨核同步。有限死锁检查按本地 Linux 的十步边界只覆盖传统等待链；不把 OFD 锁误称为有完整死锁检测。

## 固定输入与存储故障

SQLite 官方 amalgamation 为本地 `references/sqlite/sqlite-amalgamation-3530400.zip`，版本 3.53.4，SHA-256 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`；URL 和访问日期 2026-09-27 在 `references/sources.tsv`。构建不修改 SQLite 源码，保留默认 Unix VFS、线程和 WAL 编译能力，本阶段运行关闭文件映射并只验收 DELETE 回滚日志。

宿主 NBD 协议依据本地 `references/qemu/include/block/nbd.h` 与 `references/qemu/nbd/client.c`，commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。服务仅提供 fixed-newstyle/simple replies 与 READ、WRITE、FLUSH、DISC，拒绝结构化/扩展协商；不宣告 FUA、trim 或 write-zeroes。`tests/host/block_fault.c` 的 512 字节写事件先改变可见镜像，成功 FLUSH 才把先前事件写入稳定镜像。断电时服务先冻结 I/O，再按 none/odd/even/reverse 等策略选择未 flush 事件落盘，最后杀死 QEMU；不会让正常关机清理或额外 flush 改写故障结果。

进程崩溃测试让独立 `exec` 的 SQLite writer 在小缓存大事务更新后直接 `_exit`，父进程仍在同次启动中重开并触发 hot journal 恢复。断电热日志测试在大事务更新后、COMMIT 前停住并杀死客体；已确认提交测试在 COMMIT 返回后、进程关闭和内核关机前杀死客体。每次断电恢复两次，检查 SQLite `integrity_check`、行数、每行 3000 字节的完整内容和 ext4 `e2fsck -fn`。已确认事务只接受全部新值；未确认事务只接受全部旧值或全部新值，不允许行间或行内混合。正常 EXTRA/FULL 的差异单列，FULL 成功运行不能替代 EXTRA 的目录同步持久性主验收。

## 可重建验证

```sh
make test-record-lock-host
make test-files-riscv
make test-diff-abi-riscv
make test-record-lock-riscv
make test-nbd-host
make test-sqlite-rollback-riscv
make test-sqlite-nbd-riscv
make test-sqlite-recovery-riscv
make test-sqlite-recovery-matrix-riscv
```

聚焦模型测试覆盖随机区间操作、AVL 高度/最大终点、owner 索引及分配失败原子性。固定 Linux 差分同一 ELF 覆盖锁范围和错误、关闭、dup/exec/CLOEXEC、unlink、等待、信号重启与死锁；另一个静态 musl 同 ELF 入口覆盖 pthread、fork、阻塞期间 fd 复用和组退出。SQLite 正常运行的两个进程通过 `exec` 建立独立 SQLite 进程内状态；直接在已有 SQLite 连接上 fork 后继续使用该连接会复制 Unix VFS 的进程内 inode 锁缓存，不能用作独立进程争用证据。故障矩阵通过客体串口握手与 NBD `arm` 命令确认启用编号，收到 `control=arm` 后再放行客体，排除启动和关机写入，只枚举事务内写/flush 请求。原 SIGUSR1 无确认协议存在计数边界竞态，修复与重新验收见[可睡眠存储](sleepable-storage.md)。`build/` 中的镜像和日志是一次性证据，核对后用 `make prune-build` 清理；长期依据是固定输入、结果和上述重建命令。

2026-09-27 的最终矩阵输入 SHA-256：`kernel-rv` 为 `64c0a0f474af11da8824530d080e25d44493ba2f7542bfa40d68bbcf9f398084`，静态 SQLite 恢复 ELF 为 `bada7676800a46d9f15e1e2801ab1fa8a542a82b1f0b7f95a5530ac434957553`，宿主 NBD 服务为 `b838e11a094c0c34114dbca310a8a75c158a442b3130dfc440c7da6a43092e6f`；运行用 QEMU 为 11.1.1。小事务共 147 个 NBD 事件（100 次写、47 次 flush）；`make test-sqlite-recovery-matrix-riscv` 完成 441 个断电位置/策略组合、100 个写失败和 47 个 flush 失败，共 588 个故障场景，均通过两次恢复、整事务数据与 ext4 检查。`make test-sqlite-recovery-riscv` 另以同一 ELF 在固定 Linux 运行 setup/mutate/recover，其 Image SHA-256 为 `16a93ddb1d451898b93fff14de0cc076bcf1b10dad54c19a3e179a6cd81103b1`；EXTRA/FULL 正常运行、热日志、已确认提交和 FULL 错误传播均通过。

该证据只覆盖 QEMU 单 hart、所述 SQLite 事务形状及模拟故障模型。普通多进程 WAL、SMP 和实板持久性均未由此证明。

## 历史 WAL 矩阵身份（2026-09-27）

WAL 断电与共享页故障阶段证据：`make test-sqlite-wal-recovery-riscv` 在固定 Linux/BoarOS 同一 ELF 上完成 EXTRA/FULL、hot、已确认提交和写/flush 错误传播；`make test-sqlite-wal-recovery-matrix-riscv` 对 42 个事务 NBD 事件覆盖 126 个断电组合、26 个写失败和 16 个 flush 失败位置，每次两次恢复及 ext4 检查均通过。输入身份由 runner 输出：内核 SHA-256 `b1191d2737dda760a0f4ec1bc0c5ddaa1c36fe668be3584a0a3cbb02598d8f49`、恢复 ELF `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`、NBD 服务 `b838e11a094c0c34114dbca310a8a75c158a442b3130dfc440c7da6a43092e6f`、QEMU 11.1.1、SQLite archive SHA-256 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`；本地固定 QEMU v11.1.0 commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`。`make test-files-partial-write-riscv` 增加写回失败后再次标脏、写回进行中经第二个 VA 别名写入并再次同步落盘、共享 fork 元数据 OOM 扫描及固定替换失败仍保留别名/owner 的检查。`make test-diff-abi-riscv` 为 396 条一致；普通 WAL、files、userland、RISC-V 全套、栈、NBD host、lwext4 recovery、DELETE 正常与抽样矩阵和固定资料检查通过。228 项清单再次为 223/2/3，五个旧失败 ID 不变，suite identity SHA-256 `08bf67905816d479d251c9a55bf985b46dd5ef37c4f588207201d7c0e741ab82`；重建命令和完整来源见[程序清单](user-program-inventory.md)。跨 hart 真实并发与实板持久性仍未验收。

以上是该阶段历史输入，当前可睡眠存储收口结果见[可睡眠存储](sleepable-storage.md)；旧 build 路径不是永久证据。

## 通用 VFS 与 procfs 阶段复验（2026-09-28）

主线 `5f8faec` 的内核 SHA-256 为 `1a0dc5b9dc338e01d9fc7b10c689edaaa761f75952bc8fce90f2f4a4c1478167`；静态 SQLite 恢复程序为 `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`，宿主 NBD 服务为 `356cbb5d10fbe590087eda1f4bc9421f3d56c39bb4f82afd289a97fa4c18bfb1`，QEMU 11.1.1。`make test-sqlite-recovery-matrix-riscv` 通过 147 个事务事件下的 441 个断电组合、100 个写失败和 47 个 flush 失败；`make test-sqlite-wal-recovery-matrix-riscv` 通过 42 个事件下的 126 个断电组合、26 个写失败和 16 个 flush 失败。每个故障镜像均按 runner 进行两次恢复、整事务数据及 ext4 检查；普通 DELETE/WAL 和多进程 WAL 冒烟也在相同内核上通过。输入来自本地固定 SQLite 3.53.4 和 QEMU 源码清单；仍只证明上述单 hart/QEMU 故障模型。

同轮 `make test-lwext4-recovery-host` 再次通过低层事务和全部 16 种 orphan 组合及断电恢复；`make test-references` 通过。宿主模型独立于这次启动/proc 改动，不替代真实客体的逐事件矩阵。

## 2026-09-29 恢复矩阵探针口径

本轮 proc 退出修复的 WAL 完整矩阵首次在 `cut-43-none` 等待 NBD 服务结束时
超时；客体已打印 `SQLite commit confirmed`，NBD 日志只收到 42 个事务事件，
没有发生数据库恢复判定失败。原 runner 用默认持久化策略的预探针得到 43 次
（27 写、16 flush），再把 43 当作 none/odd/reverse 三种策略共同的切点上限。
隔离地从同一 `small-baseline.img` 启动三种策略、保持串口 `arm` 握手并等提交，
均得到 42 次（26 写、16 flush）；同策略的原 cut 运行也只到 42 次。
因此多出的第 43 切点在 none 策略下不可到达，旧等待方式直到 60 秒超时。
现在每种策略从自己的完整事务探针生成切点，默认策略探针仍决定写/flush
故障点，覆盖真实发生的每个请求而不把未触发的切断伪装成通过。

修正后的 `tests/sqlite-recovery-riscv.py` SHA-256 为
`f542d200cbb8bfd63abe19e45ba5152629bb006a6a599d62de5ace0c4df8c3e7`。
`make test-sqlite-recovery-matrix-riscv` 在内核 SHA-256
`ce3cbbcd511fa89732e3af53381d6a0c423ab8e6891accaf04d5f69205a3b65a`
上以 148 个 DELETE 事件完成 444 个切点、101 个写失败和 47 个 flush
失败；`make test-sqlite-wal-recovery-matrix-riscv` 以各策略 42 个 WAL
事件完成 126 个切点、26 个写失败和 16 个 flush 失败。两者均经两次恢复、
事务内容和 ext4 检查；恢复 ELF 与 NBD 服务分别为
`179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`
和 `356cbb5d10fbe590087eda1f4bc9421f3d56c39bb4f82afd289a97fa4c18bfb1`。
同轮宿主 lwext4 恢复、NBD 协议和固定资料校验通过；仍只覆盖本地单 hart
QEMU/NBD 故障模型。

## 后台写回阶段：固定启动时钟与重新枚举

后台 worker 加入后，本轮先按原 runner 全量重跑，再对最终内核重新验收。最终内核
SHA-256 为 `705f062ec1c99223b2672d7c321b813d98cabcaa4b8dedcd47a0c4cb85f21066`；
恢复 ELF、NBD 服务与 SQLite archive 仍为上文固定输入，QEMU 11.1.1、单 hart。

最终 DELETE 的默认时钟探针曾产生 155 个事件（106 写、49 flush），后续同基线镜像的
none/odd/reverse 探针均为 148（101 写、47 flush）。独立对第 106 次写注入时，客体已确认
提交，服务只收到 101 次写且没有 result=5；这不是恢复失败，而是不可达的故障位置。
因此不能将第一次探针数或上一轮的事件数量直接视为新一轮覆盖证明。

`tests/sqlite-recovery-riscv.py` 现在显式给每次 QEMU 启动设置 RTC：UTC 2030-01-01 起，
每次重启前进一小时，`clock=vm` 在本次启动内正常前进。这样原 inode 时间明确早于后续
启动，宿主墙钟与相邻重启的间距不再成为隐式 fixture 输入；不修改内核时钟或 SQLite。
固定选项依据 `references/qemu/qemu-options.hx` 的 `-rtc`，版本 v11.1.0、commit
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`。修改前保留了不可达位置的证据，修改后从相同
基线进行三次独立探针，均为 101 写/47 flush；完整矩阵仍逐策略重新探测并要求每个注入
实际触发，不把未到达位置或超时改成成功。runner SHA-256 为
`70681231dccfe307104d848bbfecc27110d04a44d6afb50ed7d9e3827d236ec6`。

```sh
make test-lwext4-host test-lwext4-recovery-host test-lwext4-rename-host test-lwext4-metadata-host
make test-sqlite-rollback-riscv test-sqlite-wal-riscv
make test-sqlite-recovery-riscv test-sqlite-recovery-matrix-riscv
make test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv
```

新 RTC fixture 下，固定 Linux 使用同一 ELF 验证 setup、EXTRA 正常提交与恢复；
BoarOS 验证 EXTRA/FULL、热日志、确认提交恢复和写/flush 错误传播。完整故障矩阵
在 BoarOS 侧运行。后台真实阈值与在途停止使用 `test-io-sleep-riscv` 的
小内存池单独触发；SQLite 小事务矩阵保护持久化契约，不能冒充后台满负载的性能证明。

最终 RTC fixture 完整矩阵均通过：DELETE 新轨迹为 148 个事件（101 写、47 flush），
三种持久化策略各 148 个切点，加 101 个写失败与 47 个 flush 失败，共 592 个故障场景；
WAL 新轨迹为 42 个事件（26 写、16 flush），三种策略共 126 个切点，加 26 个写失败
与 16 个 flush 失败，共 168 个故障场景。每个场景均实际触发注入，并完成两次恢复、
事务内容与 ext4 一致性检查。上述计数来自本轮最终内核重新探测和运行，未复用旧计数
作为覆盖证明。正常/错误入口及宿主 ext4 normal/recovery/rename/metadata 回归也通过。

## 进程身份、随机与调度阶段（2026-09-29）

RT/OTHER 调度与带宽实现接入后，重新运行正常、错误和完整恢复矩阵；没有沿用
旧事件数推定本轮覆盖。矩阵内核 SHA-256 为
`9df3c197fc0ab24b8d663c6bfcd35da515d429e1ca018cbe38d99f627cdda8a4`，
恢复 ELF `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`，
NBD 服务 `ac92fd5558bfc553f3f122c391fc32d8cbffb87fd2c6853a1149d87873d69370`，
runner `1ceb90862df125ccd112d23b363ad4c4051a4a78cf7d1b56b5e28d3cbf37bec6`，
Linux Image `0b8bcf38550399da08e98004d25056ccebf593077443a170021a5b28e26561f4`；
SQLite 3.53.4 archive 与上述固定输入相同，实际 QEMU 11.1.1。固定语义参考仍为
QEMU v11.1.0 `84f07211cc5b4fc6a371559bf8a5de4fb068e648` 与固定 Linux。

DELETE 三种断电策略分别重新枚举为 148 事件（101 写、47 flush），444 个切点、
101 个写失败、47 个 flush 失败均实际触发并通过两次恢复、事务数据与 ext4 检查；
EXTRA/FULL、hot journal、确认提交以及写/flush 错误传播也通过。
WAL 三种策略分别重新枚举为 42 事件（26 写、16 flush），126 个切点、
26 个写失败、16 个 flush 失败全部实际命中，正常/错误/两次恢复检查均通过。

最终生产快照为 `be5ca22629c904a427241b0f92e9d561d0312952e787ab75870ec4beae0143b3`。
矩阵快照之后仅增加只读 proc/MM 映像统计、消费后 boot seed 副本清零，及 RR
同时耗尽配额/时间片的同级队尾修正；上述 OTHER SQLite 工作负载没有使用 RR。
最终快照另跑 DELETE/WAL 多进程正常矩阵、第二盘 WAL 重启、普通双盘故障隔离
以及 FIFO/RR 双盘压力四组合；后者与新的 RR 边界探针专门覆盖最终调度修正。
这些是不同快照的证据，不能称完整恢复矩阵执行于最终二进制。

重建入口：`make test-sqlite-recovery-matrix-riscv test-sqlite-wal-recovery-matrix-riscv`；
每次都从当前源码构建内核并重新枚举事件，不能强制期待此处历史事件数量。
宿主 `make test-lwext4-host test-lwext4-instances-host test-lwext4-rename-host test-lwext4-metadata-host test-lwext4-recovery-host` 同轮全部通过，包含硬链接
创建/删除/替换、1024/4096 几何、extent/indirect 与 orphan file/chain 恢复。

## 独立 Review 与整次写门闩阶段（2026-09-30）

R7 将不同 OFD 的 write/writev/pwrite/append 与 truncate 按 inode 整次互斥；
fault 和 writeback 不取得该门闩，同 inode 文件映射输入的冷页等待由实际调度器回归保护。
这保证操作边界，不能据此承诺一条 write 断电原子提交。R3 的 msync 来源 pin
亦在本轮保住睡眠期间的 MM registry owner；相关契约见[可睡眠存储](sleepable-storage.md)
和[内存管理](memory-management.md)。

最终生产内核 SHA-256 `0c0c6a77f54160c06f284f19133b6bb6516f7c6e2d8dcb5f0864390ce1361fbb`；
WAL 多进程 ELF `93ce05060ea2f64cf11c5aed00e2b5a8315eda609fec8267119a88df0b5ca6fe`，
恢复 ELF `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0`，
NBD 服务 `ac92fd5558bfc553f3f122c391fc32d8cbffb87fd2c6853a1149d87873d69370`，
runner `1ceb90862df125ccd112d23b363ad4c4051a4a78cf7d1b56b5e28d3cbf37bec6`，
Linux Image `01d60a8ae733f56aa94cf11b4805da1fe876cac09d2ef81e7e7397568ee1f668`。
SQLite 固定归档仍为 `references/sqlite/sqlite-amalgamation-3530400.zip`，SHA-256
`1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`；
固定 Linux/QEMU 源码版本同上，实际 QEMU 11.1.1，单 hart 隔离 NBD 镜像。

同 ELF 的 Linux/BoarOS WAL writer 竞争与独立重启读回通过；同恢复 ELF 的固定 Linux
setup/mutate/recover 通过。BoarOS EXTRA/FULL、hot journal、已确认提交和写/flush
错误传播通过。完整 WAL 矩阵重新枚举 42 个事件（26 write、16 flush），
丢弃、保存奇数、逆序保存三种策略各 42 个切点，共 126 次；另逐点注入 26 个写失败
和 16 个 flush 失败。168 个场景均实际命中注入，完成两次恢复、整事务内容与
`e2fsck -fn` 检查，三个入口均退出 0。本轮没有重跑 DELETE 完整矩阵，历史证据仍按上节快照归属；
WAL 结果不扩大到实板掉电或吞吐/延迟结论。

可重建命令：

```sh
make test-files-partial-write-riscv test-userland-riscv
make test-io-sleep-riscv test-sqlite-wal-riscv
make test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv
make test-riscv test-glibc-riscv test-diff-abi-riscv test-scale-riscv test-stack-usage
```


## 数据路径最终恢复与宿主收口（2026-10-06）

阶段七后的首次默认 DELETE 全矩阵通过，WAL 在 `small-probe-none` 提交确认后
遇到 NBD 非零退出。原 guest 日志已确认提交，host 最后是成功 FLUSH；runner
随即 kill QEMU，可能中断正常 NBD 回复/WRITE payload。与双盘收口竞态同源，
不是已复现的 SQLite durable 失效。旧失败保留，不计作通过。

`nbd_boot` 现在统一保持控制通道；marker 和未到达序号的切断都先要求后端
`cut` 回执及退出零，再终止 guest。真实 ordinal cut 仍由后端在相应请求处冻结。
每次保存 guest/backend 退出状态、marker、cut 类型；没有放宽 NBD 错误检查。
重新运行默认和最大存储候选的完整 DELETE/WAL 矩阵，四入口均退出零：

| 配置／日志模式 | none／odd／reverse 切点 | WRITE 候选／实际注入 | FLUSH 候选／实际注入 | 未到达序号 |
|---|---|---|---|---|
| 默认 RA0/WB1，DELETE | 56／58／54 | 39／37 | 19／19 | WRITE 38、39 |
| 默认 RA0/WB1，WAL | 27／26／30 | 20／16 | 10／10 | WRITE 17–20 |
| RA8/WB8，DELETE | 57／55／60 | 40／40 | 20／19 | FLUSH 20 |
| RA8/WB8，WAL | 28／29／28 | 19／18 | 10／10 | WRITE 19 |

每例仍包括两次独立恢复、完整事务内容和 fsck；EXTRA/FULL、热日志、已确认提交
及错误传播检查均通过。未到达序号单列为提交后控制切断，不冒充实际故障注入。
异步组提交导致事件数随运行变化，不能把不同运行的序号当成同一个持久化边界。

实际 QEMU 11.1.1，恢复 ELF SHA-256
`510fc4c7b8e72a2224603b053bc9d5967f295e20a5085500038a4dd4936436f5`；
默认内核 `e3867e6b19beda83949402d51e938cf36817684e86d1e7852dfaf9977b746be7`，
RA8/WB8 候选 `ade1c0ca632c2ab1e7524daa06c52ebed49c3236113b92e01e3cb95a96628d66`；
runner SHA-256 `65d38ab9f44c6b2dc7da0cf20df4e9ebe97974ca60967326def7164953c96131`。
候选由生产源码 `faf8e6395a7d56cd92e5fdb460fd9532066d2f26` 构建，后续改动仅测试与文档；
SQLite 固定归档身份同上。该验证不承诺实板断电或吞吐收益。

```sh
make test-lwext4-host test-lwext4-batch-read-host test-lwext4-deep-truncate-host test-lwext4-recovery-host
make test-sqlite-recovery-matrix-riscv test-sqlite-wal-recovery-matrix-riscv
python3 -B tests/io-budget-experiment.py build --profiles ra8-wb8 --jobs 2
python3 -B tests/sqlite-recovery-riscv.py --kernel build/io-budget/kernels/ra8-wb8/kernel-rv --matrix full --journal delete
python3 -B tests/sqlite-recovery-riscv.py --kernel build/io-budget/kernels/ra8-wb8/kernel-rv --matrix full --journal wal
```

## LA 原 SQLite 单核交付（2026-10-07）

固定输入为 `references/sqlite/sqlite-amalgamation-3530400.zip`，SQLite3.53.4，
SHA-256 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`。
原 amalgamation 与 shell.c 使用LP64D musl1.2.5，内核继续整数ABI；每个ELF在
Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 与BoarOS共用，默认16KiB页。
512MiB/1GiB的DELETE、热日志恢复、多进程竞争、原静态/动态CLI及两次内容重启，
加WAL多进程与重启已全部通过真实wait42和根生命周期门禁。派生QEMU身份见LA模块。
这不把RV NBD断电恢复矩阵自动记为LA恢复覆盖；LA本轮验证实际正常关机独立重启。

CLI只检查integrity_check不能发现合法但丢失的行。源自本仓库的launcher增加SQL
读取已提交行及spill计数/回滚内容，两个原CLI均须返回完整精确内容。RV固定Linux
CONFIG_BLK_DEV_INITRD=n，新增共享runner不能依照LA使用initrd；实际失败的
unknown-block(0,0)启动保留后，改为已有根盘上的Linux PID1 supervisor，fork/exec
原/init、wait42、syncfs、关机。未调整固定Linux配置或原SQLite来获取通过。

最终RV NBD首次执行的SQLite程序实际通过、退出42、owner归还，backend含861次
WRITE和379次FLUSH；shell的grep -qx却不接受应用TTY行末CRLF。只还原终端CRLF
后重验完整NBD与恢复矩阵，原串口和设备请求保留；这种runner失败不能归为设备I/O
或SQLite事务失败。

```sh
make test-sqlite-rollback-loongarch test-sqlite-wal-loongarch
python3 -B tests/sqlite-rollback.py --arch riscv
make test-sqlite-nbd-riscv test-sqlite-recovery-matrix-riscv test-sqlite-wal-recovery-matrix-riscv
```
