# 真实程序清单：证据与调试经验

本文保存最近全量证据与可复用根因。待办统一在 [goals](../goals.md)，构建/执行器契约在[程序环境模块](../modules/program-environment.md)。清单生成成功只表示运行完成，不等于程序兼容或比赛成绩。

## 固定输入与复现

- BusyBox 1.33.1：`references/oscomp-testsuits` commit `b5ec6ef8497e1818cbdec3b54bb722f036e57972` 的原配置，保留全部 398 applet。
- libc-test：同一 Git 对象库中的 `8b58dd16d26d30f7c74d48d5832d870d3051b703`（选定时分支 `pre-2025`），原始 `make disk` 生成 107 个静态、110 个动态 entry；musl 1.2.5，解释器 `/lib/ld-musl-riscv64.so.1`，未修改上游 C 源码。
- 运行 Linux：`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，程序清单使用独立 `tests/program-inventory/linux.config`。BusyBox 构建单用 Linux 6.6 UAPI，原因及 SHA-256 见程序环境模块；不混用运行内核版本。
- 来源唯一清单为 `references/sources.tsv`，执行选择为 `tests/program-inventory/inputs.json`；上游源码、产物和运行日志留在忽略的 `build/`。

```sh
make inventory-userland-riscv
make test-program-inventory-host test-diff-abi-host
# 使用新的输出目录，避免覆盖旧证据；--case 可重复指定：
python3 tests/program-inventory/run.py --reuse-builds --require-pass --output build/program-check
```

双方运行同一 ELF 与同源独立 ext4 副本。`--require-pass` 使任意失败/未完成返回非零；默认仅要求清单完整生成。Linux 自己失败标为 `reference-not-pass`，需排查参考环境，不能归罪 BoarOS。原脚本内部逐项断言也必须成立，不能只看 shell 退出码；libc runtest 原本固定返回 1，直接 entry 则预期 0。

## 当前基线与阻塞

最近全量基线为 `7971eedbd3ca879f6528082d9e023ac42fadd417`，命令：

```sh
python3 tests/program-inventory/run.py --reuse-builds --suite all --output build/next-batch-inventory-final2
```

本轮文档整理核对了该目录 `runs/suite.json`：`status=complete`，211 pass、14 nonzero-exit、3 upstream-failure；这是已有运行记录，不是本轮重新跑过全量。

| 范围 | Linux | BoarOS |
|---|---:|---:|
| 顶层案例 | 228 项满足契约 | 211 项退出/完整输出双侧一致；14 项直接失败；3 项包装失败 |
| libc 静态 / 动态直接 entry | 107 / 110 全通过 | 100 / 103 通过；失败是相同七个名称 |
| 原 libc 静态 / 动态脚本 | 全部逐项断言通过 | 完整运行 107 / 110 项，各七项 FAIL |
| 原 BusyBox 脚本 | 55/55 success | 45/55 success；后台 sleep+kill 曾一过一败 |

包装脚本与直接 entry 重复覆盖，不能把 228 项或 17 个失败当成独立缺陷数。原始 stdout/stderr、wait status、串口、内核/程序/fixture 哈希及命令均在上述证据目录。BusyBox ELF SHA-256 为 `f2cda5fcdff6d41c8a553ac658e8aa55b6a48aa40898cb123a19f7865f3773ac`。

| 直接失败（均有静态/动态版本） | 基线首个有证据的阻塞 | 对应 TODO |
|---|---|---|
| stat | `/dev/null`；有效 UID/GID 查询 ENOSYS | P1a/P1c；查询完成后仍需设备 |
| syscall_sign_extend | `/dev/zero` | P1c |
| utime | `utimensat/futimens` | P1e |
| statvfs | 文件系统统计 | P1f |
| daemon_failure | `chdir("/")` ENOSYS，之后还需 `/dev/null` | P1d/P1c；后续会话依赖需继续观察 |
| pthread_robust_detach | robust futex owner-died | P2a |
| socket | socket 族与传输链 | N |

`pthread_cancel_points` 直接 entry 在基线和聚焦复跑中通过，但旧 shm_open 取消断言异常尚无独立根因，不能归功于无关的 prlimit 改动。后台 sleep+kill 也未证明稳定恢复；重复性调查仍在 P0。

## 本轮聚焦验证（不替换全量基线）

UID/GID 查询先由单测复现四项失败，同一 ELF 差分显示 Linux 返回 0、BoarOS 返回 ENOSYS；实现后新增查询和 fork/exec 检查通过，全部 185 条差分一致。原始失败保存在 `build/diff-abi/identity-before/`，修复后在 `build/diff-abi/run/`。这只完成不可变 root 查询，未解除设备或权限缺口。

随后用 `run.py --reuse-builds`，各三轮选择 `--case libc.static.pthread_cancel_points --case libc.dynamic.pthread_cancel_points --case busybox.official`，输出到 `build/review-timing-1`、`-2`、`-3`。三轮 kernel-rv SHA-256 均为 `6cf79ff99de3aec1dfe8bd6949ca8dd0133e8e9ac7061e1b46ff39b88cce49a2`，每轮保留各自 fixture 哈希、双方原始输出和 wait status；只运行这三项，其余 225 项明确 not-run。

| 观测 | 三轮结果 |
|---|---|
| 静态 / 动态取消 entry | 双方均退出 0、输出一致；旧偶发异常仍未找到独立根因 |
| 原 BusyBox 脚本 | Linux 每轮 55/55；BoarOS 每轮 45/55，完整脚本仍为 upstream-failure |
| 后台 sleep+kill | Linux 每轮 success；BoarOS 每轮先报 `/dev/null` 打开失败，再报 kill 的 No such process |

本地 `references/oscomp-testsuits` 固定 commit `b5ec6ef8497e1818cbdec3b54bb722f036e57972` 的 `busybox/shell/ash.c:forkchild()` 在无 job-control 的 `FORK_BG` 路径先关闭 stdin、打开 `/dev/null`，失败会抛出 shell 错误。它为上述日志提供直接解释：后台任务在进入 sleep 前就可能退出，与父进程 kill 形成竞争。当前证明了设备前置阻塞，尚未证明修复设备后没有其他时序问题；P1c 后要重跑，不能把旧偶尔 success 当作 sleep 正常运行。

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
| `next-batch-inventory*` | 路径/信号/资源限制阶段；`final2` 为上述最近全量 |
| `riscv/userland-run.scTdQg/static-userland.log` | stop/continue 原始失败 |

所有 Linux 源码结论均指本页开头的固定 commit。运行产物不纳入 Git；缺失旧目录时不得把文字记录当作本轮重跑结果。
