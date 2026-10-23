# 进程映像替换模块

本文描述 Linux `execve(221)` 的准备事务、共用映像策略和 scheduler 提交边界。ELF source 与缺页规则见[用户 ELF64 装载模块](user-elf.md)，任务与 MM 生命周期见[内核调度与进程生命周期模块](kernel-scheduler.md)。

## 入口与职责

| 文件 | 职责 |
|---|---|
| `kernel/exec.c`、`include/kernel/exec.h` | 捕获用户路径/argv/envp，打开可执行文件，解析并持有主程序和解释器 source，管理准备期 owner |
| `kernel/exec_image.c`、`kernel/elf_image.c` | 将 source 变成 MM、入口 PC、SP、VMA、栈和 auxv；页表操作构建期绑定 |
| `include/arch/elf.h`、`include/arch/task.h` | machine/HWCAP/trampoline 与初始、clone、exec 寄存器操作 |
| `kernel/sched/exec.c` | 撤下 CLOEXEC 后一致发布 MM 与凭据，重建 Trap Frame 并处理旧资源 |
| `kernel/exec_credentials.c` | 从实际执行 ELF 的属性计算 set-ID、文件 capability、AT_SECURE 和 dumpable |
| `kernel/syscall/process.c`、`arch/riscv/trap.c` | 解码 syscall 221 和提交动作 |

通用入口 `kernel_execve_prepare()` 在旧 MM 上建立 `PREPARED` 事务；`kernel_exec_image_prepare()` 调用共用 `kernel_elf_image_build()`。machine、页大小、MMU 和线程寄存器由构建期绑定的 RV/LA 实现提供，不使用运行期 vtable。RV 的旧入口保留薄包装；LA已通过内存reader、PCI根盘、原版LP64S静态和LP64D动态musl同一image/exec契约的双侧验收。

## 准备事务

准备顺序为：

```text
清理上次仍有 owner 的事务
-> 捕获 filename 并解析 cwd
-> 打开 regular executable OFD
-> 捕获 argv/envp 内核快照
-> 按 #! 转换解释器与 argv，最多五次
-> 建立一次解析的主 ELF source
-> 若有 PT_INTERP，解析并打开唯一解释器 source
-> 从已打开的最终主 ELF 取得 mode/owner/filecap，计算候选凭据
-> 构造新 MM、source-backed VMA、栈和含候选身份的 auxv
-> 发布 PREPARED
```

主程序支持 RISC-V `ET_EXEC` 和 `ET_DYN`；解释器支持非递归 `ET_EXEC`/`ET_DYN`。动态 source 的 `PT_DYNAMIC`/`PT_TLS` 等元数据留给用户动态链接器。主文件格式或架构错误映射为 `-ENOEXEC`；解释器路径不存在时保留路径 errno，不足64字节的解释器header按Linux elf_read返回 `-EIO`，完整header的格式或架构不正确映射为 `-ELIBBAD`；资源、I/O 和参数限制分别返回 `-ENOMEM`、`-EIO`、`-E2BIG`。解析、打开或构造失败时保留旧 MM、凭据、dumpable 和 FD 状态。

事务中的 `executable_file`/`interpreter_file` 是 source 创建前的 OFD owners；source 创建成功后由 source 持有 OFD，事务改为持有 source owner。映像构造给 MM 增加各 source 的引用，事务在提交后释放自己的引用。解析、打开或真实 VFS/I/O 清理失败时保留准确 owner；物理页、VMA metadata 和堆的合法释放完成即结束，分配器不变量错误进入 fatal。参数字符串、向量和路径的纯堆清理直接释放并清空指针，不生成 `CLEANUP_REQUIRED`；重试只处理仍持有的 MM、source 或 OFD。

## 执行凭据

主 ELF 的执行租约固定实际 inode；凭据计算从该对象取得一致的 mode/uid/gid 和 `security.capability`。脚本自身的 set-ID 位忽略，脚本展开后的 ELF 才是身份来源；`PT_INTERP` 负责加载，自己的 set-ID 或 capability 不替代主 ELF。主程序或解释器不可读时，另行降低新映像的 dumpable。

ext4 文件 capability 通过已打开 inode 读取，支持 inode 内与外部 EA，以及 revision 1/2/3 的 12/20/24 字节布局。格式错误返回 EINVAL，实际读取错误按原错误返回；initial namespace 中 rev3 的非零 rootid 按无有效属性处理。有效的全零属性仍会清除 ambient。

普通执行也把 saved/fs IDs 设为最终 effective IDs。setuid 使用文件 UID，setgid 还要求组执行位；`MS_NOSUID` 和 NNP 抑制 set-ID。文件 capability 的 permitted 组合为 `(B & file_P) | (I & file_I)`，随后按 root 兼容规则、securebits、NNP 和共享 FS 状态处理，再加入保留的 ambient。bounding 只约束 file_P 分支；带 effective 标志且所需 file_P 得不到满足时返回 EPERM。exec 清除 KEEP_CAPS 值并保留其锁定位。

`AT_UID/AT_EUID/AT_GID/AT_EGID` 来自候选身份，`AT_SECURE` 表示安全装载要求。dumpable 独立计算：旧的 real/effective 身份关系、主程序和解释器可读性、effective/fs IDs 变化以及 permitted 增益共同决定结果。两者可不同，例如 NNP 抑制文件 capability 增益后仍可能要求安全装载，而新映像可以保持 dumpable。

dumpable 保存在任务持有的小型执行安全对象中：共享 MM 的线程和 vfork 子进程共用该对象，fork 复制当前值，exec 准备独立对象并与新 MM、凭据一同发布。任务退出后立即归还页表和映射；执行安全对象保留到最后一个任务或查询借用结束，因此僵尸和已退出组长的 proc 属主仍反映共享者后续的 dumpable 修改。

准备期间，FS record 在已有 VFS 状态保护下统计共享任务：存在组外共享者时按 unsafe exec 限制权限增益；仅组内共享时登记 `exec_users`，阻止新的 CLONE_FS 加入。成功、准备失败和执行线程退出都归还该登记，早于释放线程的 FS owner。非组长 exec 保留执行线程自己的凭据和 NNP。

## 映像和初始状态

RISC-V 后端固定 Sv39/4 KiB，并支持：

- 非 PIE `ET_EXEC`，有或无 `PT_INTERP`；
- 带解释器的 PIE `ET_DYN`；
- 无解释器的 `ET_DYN`。

`PT_LOAD` 不在 exec 时复制整段文件，而登记 `ELF_PRIVATE` VMA。完整文件页使用 page cache+COW，文件/BSS 边界和 BSS 页按需建立私有页。缺页 I/O 在用户边界产生 `SIGBUS`，物理耗尽终止本次用户操作；新建可执行页完成架构转换失效和取指同步后才可取指（RV 为 `SFENCE.VMA`/`FENCE.I`，LA 为 `INVTLB`/`IBAR`）。source 的 program headers 只解析一次，避免重新读取造成 TOCTOU。

布局从随机核心产生独立的 PIE/解释器 bias、mmap base、stack、brk 和 vDSO 随机量。VirtIO RNG 累计至少 32 字节可信输入才使核心就绪；只有 DTB 材料时沿用明确的非安全降级接口，不计可信熵。完全没有材料时使用确定性布局并省略 `AT_RANDOM`。初始 PC 是解释器入口（若有），`AT_ENTRY` 始终为主程序入口；初始栈保存真实 `AT_PHDR`、`AT_BASE`、`AT_ENTRY`、`AT_HWCAP`、`AT_RANDOM`（可用时）、`AT_EXECFN` 等 auxv。入口 SP 按 RISC-V psABI 16 字节对齐，scheduler 允许入口页尚未驻留，但必须属于可执行 VMA。

## 不可返回提交

提交前 scheduler 验证当前 task、新 MM 的 `satp`、入口可执行 VMA/PTE、栈 RW PTE 和 SP 对齐。线程组 exec 先终止其他成员并等待其调用栈与资源清理完成；非组长调用者接管原 TGID 和父子树位置。竞争 exec 的未提交事务由退出路径清理。随后：

```text
旧 MM 上完成 robust-list、清 TID 并注销活跃 MM 使用者
-> 撤下所有 FD_CLOEXEC 槽，保留尚待清理的 OFD owner
-> 激活新地址空间
-> 在调度短锁内将 old MM 移入待清理 owner，同时发布新 MM 与候选凭据
-> 安装缓存地址空间信息，清零并重建 Trap Frame
-> 清理 old MM、source 和事务 owner
-> sret 到新入口
```

提交后不撤销新映像。TGID、cwd、files、fs context、`RLIMIT_NOFILE`/`RLIMIT_STACK` 和非 `FD_CLOEXEC` fd/OFD offset 保留；新栈布局按保留的软限制建立。非组长执行 exec 时调用线程接管原 TGID 作为 TID，旧 TID 释放。信号 disposition 按 exec 规则重置，F/D 状态清零。旧 MM 或 source 的真实 VFS/I/O 清理错误由持久 owner 继续处理，不改写新映像已经成功的结果。

## 验证与边界

```sh
make test-exec-riscv
make test-root-init-riscv
make test-glibc-riscv
make test-riscv
```

动态 ET_DYN/解释器的生产构造已经接入；真实 userland runner 已验证动态 musl PIE、解释器、额外 DSO、初始 TLS 和运行中 dlopen TLS。固定 glibc 2.44 的静态、动态、PIE、静态 PIE 与 pthread/TLS/信号组合另由 `test-glibc-riscv` 双侧验证。更广重定位矩阵与 `execveat` 留待后续；LA原版动态musl/初始及late DSO TLS已双侧验证。有效参数的指针表及 auxv 也计入 128 KiB 序列化限制，超限返回 E2BIG，构造不发布新 MM、也不执行 CLOEXEC；`test-elf-boundary-riscv/loongarch` 对照失败后 PID、数据及 CLOEXEC owner 保持。128 KiB 是当前序列化初始栈镜像限制，不是完整 Linux `ARG_MAX` 策略。

凭据规则的宿主直接反例使用 `sh tests/credentials-host.sh`，覆盖 4/16 KiB 页配置和候选／账户归还。`tests/workloads/credentials/exec.c` 用同一真实 ELF 检查 set-ID、filecap、auxv、安全加载、失败 exec、非组长 exec、CLONE_FS、CLOEXEC 与权限清除；`tests/credentials-fixture.py` 向独立 ext4 测试盘写入数字 UID/GID 和固定 EA 样本，并读回校验。固定 Linux 对照须启用 `CONFIG_EXT4_FS_SECURITY`，否则 ext4 不提供文件 capability 属性。

## 脚本执行

`#!` 首行读取窗口为 256 字节，空格/tab 分隔解释器路径，剩余去尾空格/tab的内容
作为一个可选参数。没有换行时解释器路径必须完整；允许截断可选参数。
每次转换删除旧 argv[0]，前置解释器、可选参数和当前脚本路径；保留 envp，
`AT_EXECFN` 保留最初用户路径。最多五次转换，超过返回 ELOOP；打开解释器保留
ENOENT/EACCES 等错误，无 shebang 文本仍返回 ENOEXEC，由用户 shell 决定回退。

脚本 OFD 在打开下一解释器前释放，释放失败仍由 exec 事务持有；字符串与向量仍使用
原有总长度限制，重组时回收被丢弃 argv[0] 的序列化空间。失败不替换 MM、cwd 或 fd。
聚焦命令为 `make test-diff-abi-riscv test-exec-riscv`；后者的缺失 `/bin/sh`
fixture 现在验证 ENOENT。BusyBox 的回退与动态加载器搜索必须另用真实 shell 验证。

深度检查发生在再次分派格式处理器之前；最后允许的脚本层仍先报告自身的 ENOEXEC 或解释器 ENOENT，不能用 ELOOP 提前掩盖这些错误。差分 `exec.script-five/six/depth-*` 覆盖边界。

首行扫描遇 NUL 停止；无换行且解释器后有分隔空格、随后为 NUL/文件结束时，可选参数存在但长度为零，仍须保留空 argv。`exec.script-empty-arg-*` 与固定 Linux 双侧验证。
