# 内存后备对象与多挂载验收

固定依据：`references/linux` commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`；内存对象与 tmpfs 主要对照
`mm/shmem.c`、`Documentation/filesystems/tmpfs.rst`、`fs/inode.c` 和
`lib/{cmdline,kstrtox}.c`，硬链接对照 `fs/namei.c`。本文早期消费者记录使用
RV64、QEMU virt、单 hart；共用挂载/syscall 路径现也由 LA 的完整 ABI 与 PTY 矩阵验证，
架构与平台范围见[LA 模块](../modules/loongarch-boot.md)。

## 基础挂载与扩展操作的边界

2026-10-08 对照 `kernel/syscall/mount.c` 与 `fs/vfs.c`：普通 proc/tmpfs/devpts/ext4
挂载和 flags=0 卸载已经接入。`tests/userland/multi_mount.c` 的 remount 输出指
成功卸载后重新普通挂载；它没有调用 `MS_REMOUNT`，不能据 marker 宣称原位重配置。

固定 `references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`fs/namespace.c` 分别处理 remount、bind、move 和传播；`include/uapi/linux/mount.h`
定义各自 flags。`ksys_umount` 先校验允许的 flags，再按 `UMOUNT_NOFOLLOW` 决定
路径跟随；`do_umount` 的 expiry、lazy detach 和后端 force 是不同操作。
BoarOS 当前只接受 `MS_RDONLY/MS_SILENT`，所有非零 umount flags 返回 `ENOTSUP`。
扩展仍未实现，非法 flags 与坏指针/路径的错误优先级也须随相应能力独立核对。

普通卸载先拒绝忙引用，再停 worker、完成 I/O 和持久化，失败保留可达挂载与真实
错误 owner。lazy detach 则需要让摘树后的实例由存活的 fd/cwd/MM 引用继续持有；
force 也不能等同于绕过用户引用或 DMA 停止确认。基础卸载的回收证据不自动覆盖
这些新生命周期，剩余任务集中在[P1h](../goals.md#p1h-虚拟文件系统与多挂载)。

## 多缓存的共同压力 owner

每实例保留 worker、快照、页和错误 owner，分配器只登记共同入口。通知与全局
脏阈值为 O(实例数)，完整快照为 O(总缓存项数)。同一个等待者只等共同一轮的
首个实际释放，或所有参与 worker 完成；等待者引用防止最后实例注销时释放
共同 owner。停止一盘不得清除另一盘回调，干净回收仍禁止进入脏 I/O。

`make test-io-sleep-riscv` 在 legacy/modern × writeback/writethrough 四组合验证：
两个非空缓存的 Cached/Dirty/Writeback 求和、低水位只扣一次、轮转释放、各自
未达脏阈值而合计触发，以及等待期间 peer 注销和 root 先释放的两种交错。
写回最终经过真实 ext4。初始测试只保留一页，导致后端 metadata ENOMEM；
改为仍处于低水位但足够事务工作的配置后，才满足“实际释放进展”的测试前提。
这不意味着 worker 有完整应急池。

## 硬链接生命周期恢复矩阵（2026-09-29）

运行 `make test-lwext4-rename-host`，由 `tests/lwext4-rename-host.sh` 编译
`tests/host/lwext4_rename.c` 和当前 lwext4 源码。几何为 1/4 KiB 块大小、
`dir_index` 开/关、`orphan_file` 开/关，共八种；原六类操作保留，新增四类：

- `unlink-alias`：删除两个名字中的一个，存活名字仍指向原 inode，nlink 从 2 降到 1，不进入 orphan。
- `rename-alias`：rename 覆盖有另一个别名的目标；被覆盖 inode 的数据和剩余别名保留，nlink 从 2 降到 1，不进入 orphan。
- `last-unlink`：先真实创建硬链接并删除原名，再删除最后别名；验证零链接 inode 的 orphan 归属以及重启后的回收。
- `orphan-reclaim`：从上一阶段已持久化的 orphan 开始，单独对数据块回收、orphan 摘除和 inode 释放注入故障。

每个操作先测量成功路径的写、flush 和分配轨迹，再逐点注入。断电包括丢弃
未 flush 数据，以及先持久化最后一个待写 512 字节扇区再断电两种情况。
write/flush 错误与分配返回 NULL 均显式断言命中，不能把未到达的注入算作通过。
下表断电列已包含两种情况；分配列统计实际注入次数，不等同于 API 返回 ENOMEM 的次数。

| 范围（每类八种几何） | 断电 | write 错误 | flush 错误 | 分配注入 | 合计 |
|---|---:|---:|---:|---:|---:|
| `unlink-alias` | 96 | 32 | 16 | 226 | 370 |
| `rename-alias` | 112 | 40 | 16 | 338 | 506 |
| `last-unlink` | 120 | 44 | 16 | 714 | 894 |
| `orphan-reclaim` | 280 | 108 | 32 | 1640 | 2060 |
| 新增 32 组 | 608 | 224 | 80 | 2918 | 3830 |
| 含旧场景的全部 80 组 | 1492 | 570 | 176 | 6562 | 8800 |

本轮 80/80 组通过。镜像内保存运行时取得的 inode 身份、已占块数和空闲计数，
恢复后同时核对目录项、内容、nlink、orphan 链及块/inode 守恒。断电和 I/O 错误
之后连续恢复两次，每个注入镜像最终均通过 `e2fsck -fn`。已释放 inode 的内容
不再属于原对象，测试只检查名字消失、orphan 摘除和空间归还，不读取其旧元数据
来推定对象仍存在。

两个验收边界需要保留。首先，底层 I/O 失败后的内存目录项可能已经摘除，重试
同一路径可以在开启事务前得到 ENOENT；错误 owner 的检查是后续事务被 EIO
拒绝、卸载失败仍保留原 mount，而不是强求路径查找覆盖所有错误。其次，orphan
回收分多个事务：截块事务已提交、后续 inode 释放事务 ENOMEM 时，允许保留
已有进展，但必须仍有零链接 orphan owner，剩余占块与空闲块守恒，重启能续做。
这四类验收没有修改生产代码。

## SQLite DELETE/WAL 恢复验收（2026-09-29）

本节按各次 runner 输出的输入身份记录结果。恢复 runner 会先复制内核、工作负载
和 NBD server，再记录副本 SHA-256；本轮 DELETE/WAL 正常恢复及两个完整矩阵
均使用以下输入：

| 输入 | SHA-256 |
|---|---|
| BoarOS `kernel-rv` | `68e20b50af3585df18e881e9829b5df4399ab78c58bec38126fcafcbe32c5a5b` |
| `build/riscv/tests/user/sqlite-recovery-rv` | `179be6d2e5ab52c908d4e0547225e7999d9e404ffd05c12f404e9f170fdca5e0` |
| `build/host/nbd-fault` | `ac92fd5558bfc553f3f122c391fc32d8cbffb87fd2c6853a1149d87873d69370` |
| `tests/sqlite-recovery-riscv.py` | `70681231dccfe307104d848bbfecc27110d04a44d6afb50ed7d9e3827d236ec6` |
| SQLite 3.53.4 `references/sqlite/sqlite-amalgamation-3530400.zip` | `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d` |

QEMU 为 11.1.1，RV64、512 MiB、单 hart，镜像为 64 MiB、4 KiB 块的 ext4。
恢复 runner 使用 VM RTC，从 `2030-01-01T00:00:00` 开始按启动次数递增一小时。
可重建入口：

```sh
make test-sqlite-recovery-riscv
make test-sqlite-wal-recovery-riscv
make test-sqlite-recovery-matrix-riscv
make test-sqlite-wal-recovery-matrix-riscv
```

前两个命令带 `--linux`，同一个 recovery ELF 在固定 Linux 上完成
setup/mutate/recover；对应 Linux 内核 SHA-256 为
`8d0f74664f715e31066a120ae86473de68c42a25360cc159da2a35dddae54dde`。
后两个命令使用 `--matrix full`，故障注入在 BoarOS 与 NBD 模型上执行，
没有执行 Linux 侧故障矩阵。

普通恢复覆盖 EXTRA/FULL 正常提交、未提交事务中止和已确认提交后的中止。
四次 recovery 运行所记录的同名普通场景计数一致：

| 场景 | DELETE writes / flushes | WAL writes / flushes | 恢复状态 |
|---|---:|---:|---|
| `extra-normal` | 1556 / 1087 | 1069 / 621 | new |
| `full-normal` | 1556 / 1086 | 1069 / 621 | new |
| `hot` | 981 / 559 | 296 / 168 | old |
| `confirmed` | 1081 / 616 | 618 / 248 | new |

两种 journal 的 `full-fail-write`、`full-fail-flush` 也均通过错误传播和恢复
检查。完整逐事件矩阵针对 runner 的 `size=1`、`sync=E` 小事务；它不表示
对所有事务大小或 FULL 模式也做了逐事件穷举。每种断电策略独立测量轨迹，
本轮 `none`、`odd`、`reverse` 三种策略各自得到相同的事件数：

| journal | 小事务事件（write + flush） | 三种策略断电次数 | write 错误 | flush 错误 | 矩阵注入合计 |
|---|---:|---:|---:|---:|---:|
| DELETE | 148（101 + 47） | 444（每种 148） | 101 | 47 | 592 |
| WAL | 42（26 + 16） | 126（每种 42） | 26 | 16 | 168 |

日志中的每种 `cut-1` 到末事件、每种 `write-fault-1` 和 `flush-fault-1` 到末点
均连续齐全，两套矩阵合计 760 次注入并通过最终验收。runner 检查断电确实触发，
I/O 错误确实返回；每个镜像连续恢复两次，SQLite `integrity_check` 返回 `ok`，
全部行只能一致为旧值或新值，每次恢复后执行 `e2fsck -fn`。

### 普通 WAL 与第二盘记录的输入边界

`make test-sqlite-wal-riscv` 的普通多进程竞争、未提交进程退出和重启验收
在 Linux/BoarOS 双侧通过。该段记录的 BoarOS 内核仍为上表的 `68e20b50…`，
工作负载 SHA-256 为
`93ce05060ea2f64cf11c5aed00e2b5a8315eda609fec8267119a88df0b5ca6fe`。

冻结后的独立 `make test-sqlite-second-disk-riscv` 在双侧通过第二个 ext4
盘上的 WAL 多进程与重启验证，其输入单独记录如下：

| 输入 | SHA-256 |
|---|---|
| BoarOS `kernel-rv` | `12085cbce0ac12ab7829c95d5e7d989c60c4380e00ca56ce66ce8c7630c88f58` |
| `build/riscv/tests/user/sqlite-second-disk-rv` | `9501b78a07afe4408684ed7b4c2b8d43b103e4d24b552e9e5b1a65c9e080e44b` |

这两次 WAL 验收都记录了上文同一 Linux 内核、SQLite archive SHA-256 和
QEMU 11.1.1。第二盘普通 WAL 使用的是另一个 BoarOS 内核，不能把根盘
DELETE/WAL 故障矩阵的 760 次注入归到这个内核或第二盘上。

同批日志还记录 rollback smoke 通过，以及 rollback 经 NBD 通过
（3793 writes、2052 flushes）；这两段没有逐项输出输入 hash，因此这里只保留
通过结果与计数，不为它们补配某个内核身份。恢复结论限于已测的 QEMU/NBD
512 字节存储模型，未覆盖实板掉电。

## 最终内存语义和消费者验收

最终生产内核为上表 `12085cbc…`。存储矩阵之后的生产变化只补 tmpfs 映射
时间通知及其单 hart IRQ 临界区，没有改变磁盘缓存、ext4、VirtIO 或恢复路径；
完整磁盘矩阵保留其真实 `68e20b50…` 输入身份。

`make test-diff-abi-riscv` 对应的最终完整差分为 **783 条全部匹配**。同一 ELF
SHA-256 为 `e9a07284eb039c7ea19274aba4af82a699646c94974b09a25a0018e8869141b6`，
清单 SHA-256 为 `c689b4e16b08d851f5fece3899ba36c92b8d7302ea104bdf8b3624b5f6127e31`；
参考 Linux image 为上文 `8d0f7466…`，配置为 `tests/diff-abi/linux.config`。
需同时启用 SHMEM 和 TMPFS：只写 TMPFS 而缺 SHMEM 时 Kconfig 会取消前者，
不能把 ramfs 回退当成 tmpfs 的差分参考。

本轮测试实际捕获并修复了三个缺口：

- 普通读空洞曾走页创建路径。新增容量检查在旧实现失败；现在 read/readv/pread
  填零且不分配文件数据页，映射缺页才实例化。
- 文件缺页发布失败曾留下新分配页。模块测试覆盖共享/私有、已有页/空洞和
  页表元数据/物理页失败；去掉 discard 后容量断言失败，恢复后全部通过。
  回滚仅撤销本次新页，已有页和其他持有者保留。
- tmpfs 直接共享写缺页未更新 mtime。六项时间差分的旧实现仅该项失败，修复后
  41 条 tmpfs 窄差分及完整 783 条通过。固定 Linux 的 `mm/memory.c`
  `fault_dirty_shared_page` 和 `mm/vma.c` `vma_wants_writenotify` 表明：读缺页
  可以直接发布可写 PTE，随后 store 不保证再次通知；私有 COW 不更新时间。
  不为统一表象额外强制写保护。测试比较显式 timespec 是否改变，不假定无 RTC
  配置下的墙钟大于指定值。时间更新无分配、无睡眠，不升级 inode 读锁。

`make test-files-riscv test-vma-riscv` 检查对象/页基线与配额守恒。files 测试在
内存文件读写、fsync/fdatasync 和映射场景包装真实块设备 read/write/flush，
断言计数均为零；这不同于仅检查同步接口返回成功。

### 组合消费者

以下项目均在最终 `12085cbc…` 内核通过：

| 入口 | 实际验收 |
|---|---|
| `make test-userland-riscv` | musl shm_open/shm_unlink，fork 与不同地址映射，最后 unlink 后保活、同名重建和忙卸载 |
| `make test-busybox-tmpfs-riscv` | 未修改的固定 BusyBox 在实际 tmpfs 上 mount、cp、cmp、mv、ln、symlink、dd、目录改名与卸载；双侧输出及退出状态一致 |
| `make test-root-multi-block-riscv` | legacy/modern 各写入与只读重启，共四次启动；任意节点名按 rdev 挂载、设备重复 claim、嵌套 tmpfs、跨挂载错误、fd/映射忙引用和最终回收 |
| `make test-multi-disk-io-riscv` | legacy/modern 与 writeback/writethrough 四组合、八次启动；暂扣 B 的真实读请求时 A 的冷读/写/fsync 与 CPU 继续进展；B 的写/flush 错误不污染 A |
| `make test-sqlite-second-disk-riscv` | 第二盘 WAL 多进程与独立重启，工作负载及输入身份见上文 |
| `make test-offline-c-tmpfs-riscv` | 工作目录及 TMPDIR 位于 tmpfs，同一 ELF 完成预处理、编译、汇编、静态链接与运行；五阶段和六个产物哈希与 Linux 一致 |

双盘故障测试还验证 B 的错误卸载保留可达 mount 和独占设备 claim，重试仍报
所属 EIO；A 已确认内容在断电重启后保留，B 可为旧值或新值，两盘恢复及 fsck
通过。故障场景由 runner 主动断电，不冒充错误盘的正常卸载成功。

BusyBox SHA-256 为 `f2cda5fcdff6d41c8a553ac658e8aa55b6a48aa40898cb123a19f7865f3773ac`，
参考 Linux image 为 `f6451cd276925903d9cc8c489ce96d9e1c60fd9d1edb0869c86d4930d0dac394`，
由 `tests/program-inventory/linux.config` 构建。该消费者复用 suite 环境检查，
需要 POSIX_MQUEUE；ABI 专用 Linux 配置缺此能力时虽然文件操作输出相同，仍应
报告 reference-not-pass，不能跳过环境检查。默认 runner 选择正确 profile。

离线 GCC 固定工具链树 SHA-256 为
`ce84a7bb9fc7c97552121b37238622bbefbd4a3672600b57374e25230c582a07`，
源码为 `5f3226afadc0a75dc9a9692baa7428fca406c00c65711b786b0994d94caf5beb`；
最终程序为 `9eb903417c06855766559ca00af19529ee85aa8a35973c2840de6ec6655e2697`，
输出 `1522623313`，输出文件 SHA-256 为
`a3f7bf4004ee05dee3c87923426f96984cb656d08bc17301a18c78910448c37f`。
为主机核对而复制到根盘的产物不表示 tmpfs 持久化。默认根盘 GCC 流程也已重跑。

最终 `make test-riscv test-glibc-riscv test-userland-riscv test-scale-riscv test-stack-usage` 通过；栈检查为 1555 个函数，最大 2368 字节，汇编 trap 288、
保留 1024。另重跑 lwext4 普通、恢复、metadata、rename 和 instances host 测试，
睡眠 I/O 四组合及固定资料恢复检查。228 项清单的最终输入与逐项结论见
[程序清单](user-program-inventory.md#tmpfs硬链接与第二磁盘阶段2026-09-29)。

实现和这些测试不代表共享文件 futex、mremap/madvise、SysV IPC、可信随机数、
完整权限或 SMP 已完成；后续候选与顺序只在 `docs/goals.md` 维护。

以上结论和重建入口入库后，先执行 `python3 -B tests/prune-build.py` 预览，再执行
`make prune-build`，本次移除 29 个运行目录、日志及过期缓存路径，保留可复用构建缓存。
