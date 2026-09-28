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

## 当前基线与阻塞

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

当前已无清单中的直接 entry 失败；剩余 BusyBox 包装器的 mount/proc/sysfs/mqueue 等环境与子命令缺口见[目标清单](../goals.md)。

`build/recoverable-metadata-focused` 另以 `--require-pass` 严格验收上述六个新增通过的 entry。`tests/program-inventory/filesystem.sh` 通过同一 `suites.run_suite()` 和未修改 BusyBox 验证 pwd、cd、指定时间 touch、文件/目录 mv 及改名后继续访问，双侧完整输出一致；manifest 与命令在 `build/recoverable-busybox-final/`。BusyBox `df` 仍失败不能解释成 statfs 未实现：独立 statvfs 与真实计数验证已通过，挂载枚举等消费者依赖继续按实际失败调查，不据命令名称补存根。

`pthread_cancel_points` 直接 entry 在当前全量和聚焦复跑中通过。`build/cancel-repeat-20260923/` 进一步以同一固定内核、Linux Image、用户输入和 runner 对静态/动态 entry 各重复 30 次：两侧每次均通过，逐次 `inventory.json`、stdout/stderr、wait status、QEMU 命令与身份在核对后已清理。每轮生成的 fixture 路径不同，因此逐轮 manifest/execution 哈希不同；输入和内核 SHA-256 一致。旧 shm_open 取消断言异常没有复现，也没有独立根因，继续保持未关闭状态；本次没有据未复现修改取消路径。

## P1b/P1c 聚焦验证

设备实现前的失败记录保存在先前清单与聚焦差分目录；实现后，真实用户程序可按 ext4 字符节点的 `rdev` 打开 `/dev/null`、`/dev/zero`、`/dev/console`，未知设备号返回 `ENXIO`。同一 ELF 的 250 条 Linux 差分全部一致，覆盖零长度、坏指针、跨页部分 fault、向量/定位 I/O、访问模式、seek、stat/rdev、poll/epoll 和 ioctl。Linux 契约核对固定 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `fs/namei.c`、`fs/char_dev.c`、`drivers/char/mem.c`、`fs/eventpoll.c` 与 `fs/read_write.c`。

路径测试覆盖绝对/相对符号链接、`.`/`..`、循环、尾斜线、最终分量跟随策略、同设备不同名称、删除后旧对象存活、同名重建取得新 inode、目录引用和 mount busy。关闭故障注入验证最后一个路径引用和重复节点合并失败时，真实 `ext4_file *` 由所属 mount 的 cleanup 队列接管并在卸载时重试；fd/OFD 测试覆盖 dup/fork 共享、阻塞 pin、close 后 fd 复用与资源回到基线。

`build/p1bc-repeat/` 对原 BusyBox 包装器和独立握手各重复 20 次。原包装器整体仍因九项无关能力缺口判为 upstream-failure，但其中后台 sleep+kill 子项 20/20 success。独立探针先由文件握手确认子进程已经 `exec sleep`，再执行 kill/wait；Linux 与 BoarOS 均 20/20 退出 0 并输出 `stage-ok`。BoarOS 有 12 次额外打印 shell 的 `Terminated` stderr，故全输出比较记录为 8 pass、12 output-mismatch；这不改变子进程进入目标阶段、被信号终止和正确回收的结论，也不用于关闭上述历史取消异常。

## 可复用的调试结论

### 先验证参考环境

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
