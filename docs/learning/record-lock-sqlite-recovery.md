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
