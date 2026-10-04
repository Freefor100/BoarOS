# 真实程序清单：证据与调试经验

本文保存最近全量证据与可复用根因。待办统一在 [goals](../goals.md)，构建/执行器契约在[程序环境模块](../modules/program-environment.md)。清单生成成功只表示运行完成，不等于程序兼容或比赛成绩。

`build/` 是未纳入 Git 的可重建产物目录，不是永久证据库。本页所列旧 `build/` 路径是当时的运行位置，2026-09-25 已清理，不能直接打开；可复用的结论、固定输入、身份和复现命令记录在 Git 中。运行器在执行期间会保存 JSON、日志和镜像，核对后用 `python3 tests/prune-build.py` 预览、`make prune-build` 清理整个一次性运行目录，只留下可跨轮复用的编译缓存。

## 固定输入与复现

- BusyBox 1.33.1：`references/oscomp-testsuits` commit `b5ec6ef8497e1818cbdec3b54bb722f036e57972` 的原配置，保留全部 398 applet。
- libc-test：同一 Git 对象库中的 `8b58dd16d26d30f7c74d48d5832d870d3051b703`（选定时分支 `pre-2025`），原始 `make disk` 生成 107 个静态、110 个动态 entry；musl 1.2.5，解释器 `/lib/ld-musl-riscv64.so.1`，未修改上游 C 源码。
- 运行 Linux：`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，程序清单使用独立 `tests/program-inventory/linux.config`。BusyBox 构建单用 Linux 6.6 UAPI，原因及 SHA-256 见程序环境模块；不混用运行内核版本。
- 来源唯一清单为 `references/sources.tsv`，执行选择为 `tests/program-inventory/inputs.json`；上游源码按清单恢复，运行日志只在核对期间暂存于忽略的 `build/`。

```sh
make inventory-userland-riscv
make test-program-inventory-host test-diff-abi-host
# 使用新的输出目录，避免覆盖正在核对的结果；--case 可重复指定：
python3 tests/program-inventory/run.py --reuse-builds --require-pass --output build/program-check
```

双方运行同一 ELF 与同源独立 ext4 副本。指定 `--case --require-pass` 时，只有本次选择集合必须全部完成并通过；未选项目保留已有结果或 `not-run`。未指定集合的严格模式要求全量完成并通过。Linux 自己失败标为 `reference-not-pass`，需排查参考环境，不能归罪 BoarOS。原脚本内部逐项断言也必须成立，不能只看 shell 退出码；libc runtest 原本固定返回 1，直接 entry 则预期 0。

## 当前基线与口径

最近的完整228项历史清单来自进程/随机/调度阶段，227 pass和原BusyBox53/55的身份
仍在下方历史记录。本轮没有重跑228项；只运行原BusyBox包装器和环境内容工作流。
原脚本两侧55/55子项成功、wait=0，dmesg保存真实启动消息，hwclock输出真实UTC。
新增内容脚本检查-r/-c/-n的作用、清空后无消息，以及df根盘总量/使用量/可用量与
同一客体statfs一致，Linux/BoarOS均退出0。BoarOS根来源为`/dev/block/252:0`，
节点由公共镜像生成器建立。环境探针另验证console不会回灌日志、RTC与realtime
一致、OFD独占/dup/fork/CLOEXEC和坏指针；新增27条使固定LinuxABI扩为1118。

原包装器含日期、日志、PID与容量等动态内容，整份stdout跨系统不要求字节相同。
清单显式选择contract比较：所有55条原断言、顺序、命令与wait status仍必须成立，
原stdout/stderr和raw_output_equal=false保留；其他案例默认仍逐字节比较。
环境内容脚本使用稳定的阶段标记，两侧输出逐字节一致。一次解析器失败来自启动
UART半行插在SUITE/ABI标记之前，重新核对同一原日志完整记录后通过，未重启重复测试。

重建：`make test-environment-riscv test-log-host test-rtc-host`；
`python3 -B tests/program-inventory/run.py --suite busybox --case busybox.official --case busybox.environment --reuse-builds --require-pass --output build/environment-check`。
输入依旧为本页固定BusyBox源码/原包装器和Linux commit；实际QEMU11.1.1、512MiB、
单hart。环境补全的结果与分类记录在本页；逐条输出、输入身份及ABI重核记录
由上述执行器在运行目录生成，大型运行记录不纳入Git。

| 分类 | 证据与判断边界 |
|---|---|
| 镜像/环境缺口 | setup/exec_errno、缺文件/节点/工具；修复环境后再判断程序，保留旧原因 |
| 辅助程序 | 固定Linux同样等待，且上游/反汇编证明须控制器或信号驱动；cgroup_fj_proc见下方证据，wait=9不改成成功 |
| 未实现能力 | 有明确ENOSYS/ENOTSUP及代码路径，区分凭据、TTY、sysfs/网卡等独立缺口 |
| 未到达 | not-run或总预算已耗尽；不称作该程序失败，也不称作通过 |
| 预算超时 | 保存预算、实际阶段、取消和wait状态；超时不证明死锁 |
| 程序错误/契约失败 | 非零、signal或原内部断言失败，先核对固定Linux及程序调用条件 |
| 性能回退 | 同身份工作量及正确完成后的分布变化；Max提高不能抵消Parent/慢任务变化 |

程序清单、ABI差分、原镜像诊断与正式评分仍是不同口径；本轮不产生正式评分。
后续主线只在[路线](../goals.md)维护，本页不积累开发任务。

## 历史阶段记录

2026-09-29 proc 线程退出生命周期修正后，以 `python3 -B tests/program-inventory/run.py --reuse-builds --output build/proc-final-inventory-20260929` 重跑全部 228 项：227 pass，唯一 `busybox.official` 仍为固定上游包装脚本失败，没有旧通过项回退。BoarOS 内核 SHA-256 `ce3cbbcd511fa89732e3af53381d6a0c423ab8e6891accaf04d5f69205a3b65a`，suite identity `fc03bfbe1bf85e5da006ad5718607bf414389905654ceed0d47eb1fbf97b8f8b`。运行身份和逐项状态由 runner 的 `inventory.json`、`runs/suite.json` 重建；它们是一次性产物，核对后清理。原 BusyBox 55 子项和 OSComp judge 使用不同口径。

2026-09-28 通用 VFS/procfs 阶段使用 `python3 -B tests/program-inventory/run.py --reuse-builds --output build/proc-inventory-20260928` 完整重跑 228 项：227 pass，唯一 `busybox.official` 为固定上游包装脚本失败，没有旧通过项回退。BoarOS 内核 SHA-256 `1a0dc5b9dc338e01d9fc7b10c689edaaa761f75952bc8fce90f2f4a4c1478167`，suite identity `e9798eac4bfef2bd6c510c3b7952ab1caa957418b3fb04d9ca2476f73b58da8c`；细目由 runner 的 `inventory.json`/`runs/suite.json` 重建，核对后不保留临时运行目录。原 BusyBox 55 子项脚本和 OSComp 原 judge 不是这 228 个顶层案例的同一个计分口径。

2026-09-28 单 hart 可睡眠 I/O 后，最终内核再次完成 228 项，227 pass、1 `busybox.official` upstream-failure，无既有通过项回退。内核 SHA-256 `5565ddce40a9ade4fac4f7a2191aa5b136b3cc456e92873ab6ef4be3abe9d8cb`，suite identity `257434c6cf372651fdb520d5599198a484ffa8fe005d04e9bf8289cc454aeedd`；命令和其余输入见[可睡眠存储](sleepable-storage.md)。

2026-09-27 单 hart 规模改动后再次完成全量 228 项，227 pass、1 `busybox.official` upstream-failure；既有通过项无回退。最终内核、suite identity、恢复验证和重建命令见[单核规模回归](single-hart-scale.md)。以下保留各阶段输入和阻塞的历史演进。

2026-09-25 全量运行使用 `build/p4d-full-20260925/`，目录已清理；复现命令：

```sh
python3 tests/program-inventory/run.py --output build/p4d-full-20260925
```

该目录 `runs/suite.json` 为 `status=complete`，228 项全部完成：223 pass、2 nonzero-exit、3 upstream-failure。与 `build/p4a-full-20260925/` 逐案例状态相同，没有已有通过项回退。本次内核增加跨 MM 共享匿名 futex，未改变原清单覆盖的 socket 与包装脚本缺口。此前 robust 阶段相对 `build/recoverable-fs-full` 的 221/4/3，让 `pthread_robust_detach` 静态/动态新增通过。

2026-09-27 记录锁与 SQLite 回滚日志阶段执行 `make inventory-userland-riscv`，`build/program-inventory-full/runs/suite.json` 再次为 `status=complete`，228 项中 223 pass、2 nonzero-exit、3 upstream-failure。五个失败 ID 精确为 `libc.static.socket`、`libc.dynamic.socket`、`libc.official.static`、`libc.official.dynamic`、`busybox.official`，与上次相同；其余 223 项均 pass，因此无旧通过项回退。该次 BoarOS 内核 SHA-256 为 `64c0a0f474af11da8824530d080e25d44493ba2f7542bfa40d68bbcf9f398084`，固定 Linux Image 为 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`，suite identity SHA-256 为 `723ad726ff2dfbf57c7a6f0641ad908a5b372349b1e6ef0b3ec3311f4d8f2631`。镜像与逐项日志在核对后按 `make prune-build` 清理，命令和身份留在此处重建。

2026-09-27 共享文件映射、`msync` 与普通 WAL 阶段通过 `python3 tests/program-inventory/run.py --reuse-builds --output build/p4bc-wal-20260927` 重跑 228 项，`status=complete`：223 pass、2 nonzero-exit、3 upstream-failure；失败 ID 与上段完全相同，无旧通过项回退。BoarOS 内核 SHA-256 为 `456c9a3ec7f3a7ab372ec53d364899100975fabf1fb6fec4e69582c3a50f0783`，固定 Linux Image 为 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`，suite identity SHA-256 为 `2ba9c58221458f62b56de468b8fca6c17489ea39e5d0333bb2e21e1d6acc619c`。另用 `make test-diff-abi-riscv test-sqlite-wal-riscv` 重建 388 条固定 Linux 差分与同一 ELF 双侧 WAL 两次启动；WAL ELF SHA-256 `93ce05060ea2f64cf11c5aed00e2b5a8315eda609fec8267119a88df0b5ca6fe`，官方 SQLite 3.53.4 archive SHA-256 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`，实际 QEMU 为 11.1.1。WAL 存储断电矩阵尚未运行，不能由回滚日志 NBD 结果推断其持久性。完整逐项输入哈希和执行命令在运行时由 `suite.json` 生成，核对后按 `make prune-build` 清理。

2026-09-27 WAL 断电与共享页故障阶段通过 `python3 tests/program-inventory/run.py --reuse-builds --output build/wal-fault-inventory` 再次全量运行，`status=complete`：223 pass、2 nonzero-exit、3 upstream-failure；失败 ID 仍是 `libc.static.socket`、`libc.dynamic.socket`、`libc.official.static`、`libc.official.dynamic`、`busybox.official`，无旧通过项回退。BoarOS 内核 SHA-256 为 `b1191d2737dda760a0f4ec1bc0c5ddaa1c36fe668be3584a0a3cbb02598d8f49`，固定 Linux Image 为 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`，suite identity SHA-256 为 `08bf67905816d479d251c9a55bf985b46dd5ef37c4f588207201d7c0e741ab82`。同一阶段另以 `make test-diff-abi-riscv test-sqlite-wal-riscv test-sqlite-wal-recovery-riscv test-sqlite-wal-recovery-matrix-riscv test-files-partial-write-riscv` 重建 396 条差分、普通 WAL 和逐事件存储故障证据；具体 NBD 输入身份与边界见[恢复证据](record-lock-sqlite-recovery.md#历史-wal-矩阵身份2026-09-27)。

2026-09-27 glibc/futex bitset 阶段通过 `python3 tests/program-inventory/run.py --reuse-builds --output build/glibc-futex-inventory` 再次全量运行，`status=complete`：223 pass、2 nonzero-exit、3 upstream-failure；仍是上述五个失败 ID，无旧通过项回退。BoarOS 内核 SHA-256 为 `ebc11763ddac2661df3af45cca96ca322abeba0623e5cb9919a9051392db4574`，固定 Linux Image 为 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`，suite identity SHA-256 为 `5419a733de0e8e5095f53876f261a5a17c0c9f49b6f445dc0953e6fcef48c9c9`。同一阶段的 `make test-glibc-riscv test-diff-abi-riscv test-userland-riscv test-sqlite-wal-riscv test-riscv test-stack-usage` 通过，固定 Linux 差分扩至 408 条；`make test-sqlite-wal-recovery-riscv test-offline-c-baseline-riscv` 也在整合后的内核上通过。glibc 基础矩阵通过不改变既有 socket 缺口，也不代表离线编译已完成。

2026-09-27 离线 C 编译整合后通过 `python3 tests/program-inventory/run.py --reuse-builds --output build/offline-gcc-inventory` 重跑全部 228 项，`status=complete`：223 pass、2 nonzero-exit、3 upstream-failure；失败仍是上述五项，没有旧通过项回退。BoarOS 内核 SHA-256 为 `9f848c4b74aa8415c0869616abfccd456e26742e1959d717b7f59f57f50164c4`，固定 Linux Image SHA-256 为 `16a93ddb1d451898b93fff14de0cc076bcf1b10dad54c19a3e179a6cd81103b1`，suite identity SHA-256 为 `f3540ed6f317d7ca605780c938e4dbd6b66f76af56836e128234018f7c9329a4`。`make test-diff-abi-riscv` 的 430 条记录双侧一致；`make test-offline-c-riscv` 在同一内核上完成固定 Alpine GCC 的五阶段客体内编译，产物哈希见[离线工具链记录](offline-toolchain-probe.md)。镜像与逐项日志核对后按 `make prune-build` 清理。

2026-09-27 IPv4 loopback 首切片在全量基线之后单独重跑原版 `libc.static.socket` 与 `libc.dynamic.socket`：固定 Linux 与 BoarOS 四次执行均 `pass`，原始输出与 wait status 一致。聚焦 suite identity SHA-256 `cd6490b7cbab34d77f08407a0295461a68f4e83e0e48f011c27ddc874bcbc51e`；BoarOS 内核 `bb09a7969d92cdc3a0f996af17832452c1728931f31f215d6b58c382b183845e`，固定 Linux Image `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`，未修改静态/动态入口 ELF 分别为 `d7669dcc49c75a9e1eb1c2d5896a4def35bd5d880025d9386b4eba1285781def`、`f092124714fc1fa6f57d353150e1c0896833ab72fd6d012761914bd0f7816d1b`。聚焦运行只读复用主 checkout 已核验构建输入，命令等价于[程序环境模块](../modules/program-environment.md)的 `--suite libc --case` 双入口；后续整合全量结果如下。

2026-09-27 IPv4 loopback、审查修复与离线编译整合后执行 `python3 tests/program-inventory/run.py --reuse-builds --output build/socket-final-inventory`，`runs/suite.json` 为 `status=complete`：228 项中 227 pass、1 upstream-failure。仅 `busybox.official` 仍失败；原 `libc.static.socket`、`libc.dynamic.socket`、`libc.official.static`、`libc.official.dynamic` 均转绿，旧 223 个通过项未回退。BoarOS 内核 SHA-256 `012e24e42718f8850301d3855a96e6975ea20d685951a60e32731fddb60824ef`，固定 Linux Image SHA-256 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`，suite identity SHA-256 `044fdf1d02bcda873688cf3a38bd6c51ce289384db7f4681b8555effbc0440e2`。同一最终内核的 `make test-diff-abi-riscv` 为 502/502 条一致，`make test-lwip-host test-userland-riscv test-files-riscv test-syscall-riscv test-stack-usage test-riscv test-glibc-riscv test-sqlite-wal-riscv test-sqlite-wal-recovery-riscv test-offline-c-riscv` 通过。BusyBox 原脚本双侧分别为 55/55 与 51/55 success，BoarOS 未通过 df、dmesg、free、hwclock；旧记录中的 `which ls` 已转绿。核对时从本次 `suite.json` 读取完整输入、逐项状态与 QEMU 命令；镜像和日志核对后运行 `make prune-build`。

| 范围 | Linux | BoarOS |
|---|---:|---:|
| 顶层案例 | 228 项满足契约 | 227 项通过；1 项 BusyBox 包装失败 |
| libc 静态 / 动态直接 entry | 107 / 110 全通过 | 107 / 110 全通过 |
| 原 libc 静态 / 动态脚本 | 全部逐项断言通过 | 全部逐项断言通过 |
| 原 BusyBox 脚本 | 55/55 success | 51/55 success；df、dmesg、free、hwclock 仍失败 |

包装脚本与直接 entry 重复覆盖，228 个顶层案例不是 228 个独立能力。当前唯一未通过项是 BusyBox 原脚本，其中四条子命令断言失败。早期基线的内核 `kernel-rv` SHA-256 为 `48779733d5fedb7419755e4e8244b4d2fd433832fb5dc97cd5585a0b9130b619`，固定 Linux Image 为 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`；这些旧身份仅供历史定位。最终整合身份与重建命令见上段。

当时已无清单中的直接 entry 失败；该历史阶段的 BusyBox 包装器环境与子命令缺口见[目标清单](../goals.md)。

`build/recoverable-metadata-focused` 另以 `--require-pass` 严格验收上述六个新增通过的 entry。`tests/program-inventory/filesystem.sh` 通过同一 `suites.run_suite()` 和未修改 BusyBox 验证 pwd、cd、指定时间 touch、文件/目录 mv 及改名后继续访问，双侧完整输出一致；manifest 与命令在 `build/recoverable-busybox-final/`。BusyBox `df` 仍失败不能解释成 statfs 未实现：独立 statvfs 与真实计数验证已通过，挂载枚举等消费者依赖继续按实际失败调查，不据命令名称补存根。

`pthread_cancel_points` 直接 entry 在当前全量和聚焦复跑中通过。`build/cancel-repeat-20260923/` 进一步以同一固定内核、Linux Image、用户输入和 runner 对静态/动态 entry 各重复 30 次：两侧每次均通过，逐次 `inventory.json`、stdout/stderr、wait status、QEMU 命令与身份在核对后已清理。每轮生成的 fixture 路径不同，因此逐轮 manifest/execution 哈希不同；输入和内核 SHA-256 一致。旧 shm_open 取消断言异常没有复现，也没有独立根因，继续保持未关闭状态；本次没有据未复现修改取消路径。

## P1b/P1c 聚焦验证

设备实现前的失败记录保存在先前清单与聚焦差分目录；实现后，真实用户程序可按 ext4 字符节点的 `rdev` 打开 `/dev/null`、`/dev/zero`、`/dev/console`，未知设备号返回 `ENXIO`。同一 ELF 的 250 条 Linux 差分全部一致，覆盖零长度、坏指针、跨页部分 fault、向量/定位 I/O、访问模式、seek、stat/rdev、poll/epoll 和 ioctl。Linux 契约核对固定 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `fs/namei.c`、`fs/char_dev.c`、`drivers/char/mem.c`、`fs/eventpoll.c` 与 `fs/read_write.c`。

路径测试覆盖绝对/相对符号链接、`.`/`..`、循环、尾斜线、最终分量跟随策略、同设备不同名称、删除后旧对象存活、同名重建取得新 inode、目录引用和 mount busy。关闭故障注入验证最后一个路径引用和重复节点合并失败时，真实 `ext4_file *` 由所属 mount 的 cleanup 队列接管并在卸载时重试；fd/OFD 测试覆盖 dup/fork 共享、阻塞 pin、close 后 fd 复用与资源回到基线。

`build/p1bc-repeat/` 对原 BusyBox 包装器和独立握手各重复 20 次。原包装器整体仍因九项无关能力缺口判为 upstream-failure，但其中后台 sleep+kill 子项 20/20 success。独立探针先由文件握手确认子进程已经 `exec sleep`，再执行 kill/wait；Linux 与 BoarOS 均 20/20 退出 0 并输出 `stage-ok`。BoarOS 有 12 次额外打印 shell 的 `Terminated` stderr，故全输出比较记录为 8 pass、12 output-mismatch；这不改变子进程进入目标阶段、被信号终止和正确回收的结论，也不用于关闭上述历史取消异常。

## 可复用的调试结论

### 从原ELF到内核，逐层核对失败归属

程序名和errno不能单独决定归因。先检查测试构建时是否编入目标分支，再核对libc
包装器实际传给内核的系统调用与参数、内核支持集合，最后比较断言和参考版本。
LTP 20240524 的 `ksm02`、`io_cancel02` 有缺少libnuma/libaio开发依赖时直接TCONF的
条件编译分支；内核新增功能或运行时安装库，不能开启原ELF中已被编译掉的测试。
固定来源为 `references/oscomp-testsuits` 的pre-2025树中对应源码。

原比赛镜像的musl `epoll_create` 包装器先将参数置零，再调用 `epoll_create1(0)`，
丢弃了size；负size断言失败时，内核接收的是合法flags=0。原二进制应通过只读镜像
提取与反汇编核对，不能拿另一个版本的musl源码代替实际输入，也不能在内核按程序名
补回丢失参数。该输入问题不否定同一程序其他有效的epoll断言。

旧LTP的 `epoll_ctl04` 要求嵌套过深返回EINVAL，而固定Linux 7.2
`references/linux/fs/eventpoll.c` 的 `ep_loop_check` 失败路径返回ELOOP。记录版本差异，
再决定是否存在内核错误；不能为了旧断言改变已经符合固定参考的errno。

### LTP的准备依赖与libc边界

固定比赛输入中的LTP为20240524，来源是 `references/oscomp-testsuits` 中由
`tests/program-inventory/inputs.json` 选定的pre-2025树；不能把默认分支源码当成
原镜像二进制。Linux比较依据仍为本地Linux 7.2，精确对象由来源清单管理。
比赛脚本无参数遍历bin目录，上游runtest则选择程序、参数、控制器和环境，二者覆盖不同。

先保存目标测试之前的失败。`lib/tst_tmpdir.c` 的公共准备调用
`chown(TESTDIR, -1, getgid())`，而RV64路径需要fchownat。该准备依赖已由
真实ext4/tmpfs所有权修改补齐；进程仍固定root。它只解锁准备，不证明后续权限、
映射或信号断言通过。文件uid/gid由inode持有，进程凭据是另一个owner；不能用返回0或
创建nobody账户冒充完整权限。原镜像缺账户时，`symlink03`、`mlockall03` 的旧setup
还会在没有检查getpwnam结果的情况下读取pw_uid，用户SIGSEGV应先查这个调用链。

原musl的两个包装行为尤其容易误判：

- `sigtimedwait` 在raw返回-EINTR后循环重试，`sigwaitinfo`以NULL timeout调用它。
  原libc二进制中的重试分支与固定musl 1.2.5源码一致。LTP的
  `libs/libltpsigwait/sigwait.c::test_empty_set`却要求空集合等待被SIGUSR1打断并
  向应用返回EINTR；发送子进程在测试返回之前持续发信号。即使内核正确打断，包装器
  也会重新等待，直到原watchdog终止。保留失败，不把它记作调度死锁；raw syscall的
  EINTR、匹配信号消费及剩余子测试仍用有效输入独立保护。
- `clone04`要求libc对NULL child stack返回EINVAL。原旧musl直接进入汇编
  `__clone`，先在stack-16保存函数和参数，再执行ecall；NULL输入在进入内核clone
  前就触发用户fault。LTP也标注了对应musl修复。不能拿新的libc源码覆盖这个事实，
  更不能让内核把所有raw clone的NULL栈一概拒绝，破坏合法fork式调用。

glibc线程退出和取消可能在运行期加载 `libgcc_s.so.1`。缺少匹配unwind库时会由libc
主动abort；程序已加载不代表依赖完整，随意复制不同libc工具链的libgcc也不是修复。
静态cancel-points的join结果与动态缺库分别核对，不用一个解释覆盖所有取消异常。
旧clock_gettime断言同时要求成功返回和errno为0，且没有先清errno。原静态glibc
启动会请求8字节的`getrandom(..., GRND_NONBLOCK)`；没有可信RNG、熵尚未就绪时
内核应返回EAGAIN，libc可以留下该errno。成功的时钟调用不承诺清除历史errno。
同内核、原ELF的有/无VirtIO RNG对照区分了这个环境与断言边界；不是RTC未实现，
也不能由内核伪造熵或成功时清errno来迎合测试。固定行为见
`references/linux/drivers/char/random.c`和BoarOS的`kernel/syscall/random.c`。
临时诊断只在忽略的build通过链接包装器核对请求及返回，不加入生产打印或观测。

比赛镜像中的 glibc 是 Ubuntu 2.35，不能用项目另行固定的 glibc 2.44 源码来声称
它的全部行为。libc-test 的 C locale、扫描／格式化、正则和取消断言有些反映
musl 的实现选择；同一原 ELF 在固定 Linux 上仍出现的失败，先归入库／输入边界，
再核对标准要求，不能让内核伪装返回值去消除分差。

libc-bench 的 `utf8.c` 依次尝试六个 UTF-8 locale；若 `CODESET` 仍不是 UTF-8，
直接返回，而 `main.c::run_bench` 忽略函数返回值、照常打印时间。旧 glibc 镜像缺少
这些 locale 时，短耗时表示转换工作未执行。要评价字符转换成本，必须同时确认 locale
成功及实际工作量，不能把跳过工作算成库的速度优势。`/proc/self/smaps` 未提供相应
字段时，bench 输出的零内存值同样不代表没有分配。

`shmat1`在上游 `runtest/mm` 因无限循环问题禁用：每批线程有次数上限，外层却
反复建批，读写与附加之间缺少完成握手，24小时alarm的exit0不证明线程工作完成。
`shm_test`则是有限压力：默认30线程、1000轮、逐字节yield，上游用较小的线程/轮数
组合。大量循环本身不是缺陷；无控制器helper、上游缺陷、资源压力与有效长任务分开，
只有明确的输入角色才能支持跳过。补跑改变前置状态，不能拼接为一次正式成绩。

### 性能结果必须对应实际工作

原judge的内嵌baseline不是当前固定Linux、同镜像和同QEMU下新测的对照。正值、
分数或某个Max只能提示调查方向，不能证明整个系统效率；原脚本无逐命令时间时，
也不能把全组耗时全部归给其中一个子项。

| 负载 | 实际路径与正确读法 |
|---|---|
| lmbench lat_fs | `lmbench_src/src/lat_fs.c` 输出创建/删除次数每秒；0k执行creat/close/unlink，不写文件内容。慢路径要分解目录查找、inode/位图、事务准备、orphan及真实资源等待，数据复制不是唯一候选。 |
| 路径stat/open与fstat | 前者包含用户路径导入、路径解析和元数据，后者使用已打开的对象；差距不能直接当磁盘带宽。glibc的fstat可以经由newfstatat的空路径进入内核，raw fstat或另一libc的成本不能直接替代该路径。空路径先识别再取得对象，避免每次准备完整路径容量；热缓存/冷设备、同/不同目录和后台事务状态分别控制。 |
| iozone | 固定工作量完成、应用要求的durable、后台checkpoint排空分别记录；多进程停止规则和实际传输量影响Parent/Max/Children，零长度元数据成本不能由大块缓存写吞吐替代。 |
| cyclictest | 原脚本使用1ms间隔；judge取各线程最大值再平均，不是平均每次唤醒延迟。普通sleep deadline尚未纳入额外SBI重装，先区分到期检查、ready等待与IRQ-off，不把细粒度读时钟等同及时唤醒。 |
| iperf/netperf | 原iperf UDP使用1000G目标，属于过载；按实际接收、丢包、每连接进展判断。STREAM是字节吞吐，RR/CRR是事务速率；不同参数和baseline使两程序分数不能互相代替。 |

当前socket预算与lwIP协议预算不是同一个限制。生产 `lwipopts.h` 的TCP_WND和
TCP_SND_BUF均为8个1460字节MSS；仅增加SO_SNDBUF不能自动扩大协议窗口。池压力、
背压后的复制/重试与任务运行机会是候选，需固定内容和全部任务完成后归因；loopback
成绩不代表网卡或公网能力。TCP_INFO尚未提供，原应用的Retr/Cwnd字段不作为可靠统计。

CPU百分比也要说明记账边界。`times`的U/S时间目前按timer中断当时状态采样；反复
短syscall仍可能没有足够S-mode样本，不能因此称内核工作为零。优化一次只选择一个
有证据的机制，并核对端到端完成、响应和资源代价；待办统一维护在goals。

### 先验证参考环境

工具输出成功不等于同步发生。libc的`sync()`没有返回值，即使底层syscall尚未接入，
工具也可能退出0；因此持久化探针应检查raw sync/syncfs或fsync返回值，并在独立后端／
重启视角核对内容。已接入的syncfs按fd所属挂载交接数据并等待durable；全局sync继续
尝试所有挂载而不返回I/O错误，错误保留给所属挂载和后续同步观察。匿名对象的syncfs
不触及根盘，不能借这种成功证明磁盘内容持久化。

固定 Linux 的 `init/do_mounts.c` 在 `/init` 前挂载 devtmpfs，会遮住镜像中原有 `/dev/shm`。初次 `pthread_cancel_points` 的 shm_open 因此失败；错误诊断中的 write 又成为 pending cancellation 的取消点，隐藏了原错误。补齐可见目录和 tmpfs 后同一 Linux/ELF 通过。socket 访问 loopback 前同样需要真正启用接口。环境 setup 失败不算内核 ABI 差异。

完整 BusyBox 构建缺 `linux/kd.h` 是目标 UAPI 未导出；`tc` 的旧 CBQ 定义则需固定 6.6 UAPI。分别解决工具输入，保留原配置，不以裁剪 applet 回避缺口。默认决赛 commit 没有 libc-test，只能说明该树的内容；完整对象库中可取得固定 `pre-2025` 输入。

### 加载后失败不一定是动态链接缺失

动态 argv 返回时，GDB 在应为 BSS 的 `t_status` 读到 `0x615f696e`，对应 ELF 非装载尾部 `ni_array\0.data.r`；程序已经进入 main。`kernel/elf64_source.c` 未把非对齐文件尾页与完整文件页分开，导致 BSS 未清零。`test-elf-tail-riscv` 的同一真实 ELF 修复前 Linux PASS / BoarOS FAIL，修复后双方 PASS；保留三整页、非对齐尾和多页 BSS 探针。

### 完整退出输出不等于资源清理完成

静态 libc 包装器曾打印完整 `SUITE END`，随后 root finish 报错。固定内核/fixture 的 20 次重复在第 11 次复现：`stage=0x40`，heap 已清空但少一物理页。PID 1 的未等待 zombie 被追加到退出队列尾，idle 看到 PID 1 completion 就提前停止回收。真实 U-mode 孤儿 zombie 探针先稳定失败，排空队列后通过；修复后同场景 20 次完成。owner 仍是队列，不能把它归成 libc syscall 缺失或分配器重试问题。

### 资源限制要检查生效点和输出故障

固定 Linux `kernel/sys.c:do_prlimit/prlimit64` 先导入新值、在目标线程组快照旧值并提交、最后复制旧值；输出 EFAULT 不回滚设置。`fs/file.c` 在分配新 fd 时查 NOFILE；`mm/vma.c` 在扩栈时查 STACK。BoarOS 使用预留 VMA+lazy fault，须保持可提高软限额的地址空间，不能在 exec 时永久缩小 VMA。`tests/diff-abi/limits.c` 的 exec 后提高 STACK 并访问 1 MiB 栈曾证伪旧实现；65535 字节非页对齐限制又发现初始预映射未对齐。两者均保留差分验收。小栈探针还会受 Linux 已扩展 VMA 影响，不可凭一次溢栈就推定全部边界相同。

### 等待事件的测试也会竞争

真实 U-mode stop/continue 探针曾偶发返回 74；子进程 SIGCONT 后立即退出，可能早于父进程观察 WCONTINUED。当次未记录逐项 wait 返回，不能称已证明唯一根因。测试改为子进程持续 yield，父进程观察继续事件后再 SIGTERM；这样保护继续事件本身，避免自然退出干扰。重复通过仍不证明所有时序正确。

## 历史证据索引

旧计数不作为当前能力，只保留定位价值与产物入口；详细演进由 Git 历史保存。

| 证据目录（均在 `build/`） | 用途 |
|---|---|
| `program-inventory-final`、`program-environment/reproducibility.json` | 首次完整 226 项与两次 BusyBox 构建身份 |
| `program-dynamic-probe`、`elf-stage-dynamic` | BSS 现场及尾页修复后动态 entry |
| `readv-inventory-verified` | ELF/readv 后 228 项复跑，含独立 od/hexdump |
| `readv-inventory-final`、`root-finish-repeat`、`root-finish-fixed-repeat` | root finish 原始失败、复现与 20 次修复验证 |
| `signal-wait-wrapper` | rt_sigtimedwait 后原包装脚本完整记录 |
| `prlimit-focused`、`recheck-cancel-sscanf` | 限额相关 entry 与取消复跑，不能据此关闭旧取消异常 |
| `next-batch-inventory*` | 路径/信号/资源限制阶段；`final2` 为历史 `7971eedb` 全量 |
| `p1bc-full-final3` | P1b/P1c 后历史 215/10/3 全量、身份和双侧日志 |
| `recoverable-fs-full`、`recoverable-metadata-focused`、`recoverable-busybox-final` | 文件系统阶段 221/4/3 全量、六个严格 entry 及 BusyBox 组合 |
| `robust-full-20260923`、`robust-focused-first`、`cancel-repeat-20260923` | robust 阶段 223/2/3 全量、两项真实 entry 与静态/动态取消各 30 次复跑 |
| `p4a-full-20260925` | 共享匿名阶段 223/2/3 全量，逐案例状态与 robust 阶段相同 |
| `p4d-full-20260925` | 跨 MM 共享匿名 futex 阶段 223/2/3 全量，逐案例状态与 P4a 阶段相同 |
| `p1bc-repeat` | 原 BusyBox 包装器与同步 sleep+kill 各 20 轮 |
| `riscv/userland-run.scTdQg/static-userland.log` | stop/continue 原始失败 |

所有 Linux 源码结论均指本页开头的固定 commit。运行产物不纳入 Git；缺失旧目录时不得把文字记录当作本轮重跑结果。

## 内存消费者与 LTP 等待独立诊断（2026-09-29）

`python3 -B tests/program-inventory/run.py --reuse-builds --output build/writeback-final-inventory`
重新遍历全部 228 项，227 pass、1 upstream-failure，与原包装器缺口一致。
最终内核 SHA-256 为 `705f062ec1c99223b2672d7c321b813d98cabcaa4b8dedcd47a0c4cb85f21066`，
清单执行身份 SHA-256 为 `a8c8ea57bd7440c80e7c0fc92a509dea18b88a617a614ed2330904bfe77b5b85`。
原镜像诊断命令和输入 SHA-256 见[时间定位](file-timestamps.md#原-rv-镜像-utime-的独立定位2026-09-29)，
不合并成正式成绩。原 BusyBox free 输出例如 total=523116、free=520232、buff/cache=328、
available=509776 KiB；这来自真实 meminfo/sysinfo，退出零并非唯一验收条件。
原 LTP abort01 越过 Cached 查询后首次停在
`tst_tmpdir.c:287: chown(/tmp/LTP_...,-1,0) failed: ENOSYS (38)`，wait 状态为 512（exit 2）。

原 `/glibc/ltp_testcode.sh` 遍历 bin 中每个普通文件并无参数执行。隔离运行
`/bin/sh /glibc/ltp/testcases/bin/cgroup_fj_function.sh` 两侧 exit 6，缺少 subsystem 参数；
BoarOS 还明确报 setpgid ENOSYS。另传 cpuset 时两侧 exit 32，BoarOS 首个能力阻塞为
`Kernel does not support control groups`。进程组与控制器能力分别归后续阶段。

`cgroup_fj_proc` SHA-256 为
`f6894edfc176874ef0a9d7c395fae49473967c3977edd29023dc34a284ab8570`。
`objdump -d --disassemble=main` 的 0x880–0x8a0 注册 SIGUSR1 handler 后进入 sigsuspend；
随后才开始 fork/wait 循环。无参数独立运行在 Linux 和 BoarOS 均耗尽 3 秒诊断预算，
SIGKILL 后 wait 得到 9，两侧回收正常。因此该辅助程序被无控制器/信号驱动的总 runner
单独执行会消耗剩余预算；此次未复现内核 wait 死锁。不能据此声称完整 cgroup 或所有信号路径已兼容。

诊断环境先安装原 BusyBox applet（独立 60 秒预算），再显式提供 shell 链接；初版 5 秒安装预算
曾造成不完整 PATH，这不是 cgroup 的产品缺口。最终复现确认 install 正常结束后才解释子项失败。

## tmpfs、硬链接与第二磁盘阶段（2026-09-29）

冻结实现后运行：

```sh
python3 -B tests/program-inventory/run.py --reuse-builds --output build/multimount-frozen-inventory
```

全部 228 项执行完成，227 pass、1 upstream-failure（`busybox.official`）。
逐项状态与本阶段前一次完整清单相同，没有把“总数相同”代替逐项检查。
原 BusyBox 包装器缺口保持单列；另增的 tmpfs BusyBox 消费者不混入这 228 项。

最终 BoarOS 内核 SHA-256：`12085cbce0ac12ab7829c95d5e7d989c60c4380e00ca56ce66ce8c7630c88f58`。
固定 Linux 内核 SHA-256：`f6451cd276925903d9cc8c489ce96d9e1c60fd9d1edb0869c86d4930d0dac394`。
清单执行身份 SHA-256：`5e08f6fa28aba24743c11b16c03e389d40df4e0fe84c82f3f252165a1fb58efe`。
Linux 使用 `tests/program-inventory/linux.config`；组合消费者和恢复范围见
[内存后备对象与多挂载验收](memory-backed-mounts.md)。这是主线诊断，不是正式评分。

## 进程身份、随机与调度阶段（2026-09-29）

`python3 -B tests/program-inventory/run.py --reuse-builds --output build/process-phase-inventory`
重新执行全部 228 项，结果为 227 pass，只有 `busybox.official` 为 upstream-failure。
核对完整 manifest 的 228 个唯一 ID 与实际结果集合相等，并逐 ID 对照前阶段记录：
该包装器之外所有项目都为 pass，没有跳过、新增失败或用相同总数替代逐项检查。
原包装器仍不算通过；独立 setsid/chrt/taskset/daemon、iperf/cyclictest 诊断见
[原程序调用链](session-consumers.md)，不混入这 228 项或拼成正式评分。

BoarOS 内核 SHA-256 `be5ca22629c904a427241b0f92e9d561d0312952e787ab75870ec4beae0143b3`；
固定 Linux Image `9d6a69758e26400a14b13cd053afdb7db8c98bd797d9eb7d3ad4b1c0a108e413`；
suite identity `aa3bda0115670de7a837a7331dd563c9d282991109d1b6f17f39ecc7a8346fcd`。
Linux 清单配置同步启用 HWRNG 与 proc sysctl，输入仍为固定原 BusyBox/libc-test。
最终内核另通过 RISC-V 全套、规模、睡眠 I/O 四组合、userland、glibc 五形态、
RNG 八次生命周期启动、调度 host/U-mode、SQLite DELETE/WAL 正常与第二盘重启、
普通双盘故障隔离与实时双盘四组合、ext4 宿主正常/错误/恢复矩阵。
固定离线 GCC 在 ext4/tmpfs 的五阶段、产物和输出也双侧一致，工具链树仍为
`ce84a7bb9fc7c97552121b37238622bbefbd4a3672600b57374e25230c582a07`。

`tests/userland-riscv.sh` 的每次启动预算改为可配置 `USERLAND_TIMEOUT`（默认 120s）；
旧 30s 预算在 session 独立阶段与组合阶段均出现超时，较长预算下完整标记和资源验收通过。
保留全部输出/退出/回收断言，不把超时当成功，也不从受调试暂停影响的历史运行推算性能。
对应重建：

```sh
make test-riscv test-scale-riscv test-io-sleep-riscv test-stack-usage \
  test-userland-riscv test-glibc-riscv test-rng-riscv test-sched-policy-host \
  test-sched-bandwidth-riscv test-multi-disk-io-riscv test-multi-disk-rt-riscv \
  test-sqlite-rollback-riscv test-sqlite-wal-riscv test-sqlite-second-disk-riscv \
  test-root-multi-block-riscv test-offline-c-riscv test-offline-c-tmpfs-riscv
```

静态栈分析覆盖 1638 个函数，最大 2368 字节；trap 288 字节、预留 1024 字节。
调度用户探针释放 12 个任务栈、最小余量 5152 字节，退出 heap-live=0。

最终 `tests/runtime-diagnostics.py --output build/process-final-runtime-diagnostics
--compat-release 4.15.0` 独立复跑了上述 LTP 链：setpgid 的 ENOSYS 已消失；
无 subsystem 的 function 仍 exit 6，传 cpuset 仍因没有控制器 exit 32；
无信号驱动的 helper 两侧仍在 3 秒后 SIGKILL/wait=9。abort01 的 BoarOS
首个失败仍为 chown ENOSYS、exit 2。隔离变体身份与原 utime 结果见[文件时间](file-timestamps.md)。
