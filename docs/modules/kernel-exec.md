# 进程映像替换模块

本文描述 Linux `execve(221)` 的准备事务、RISC-V 映像后端和 scheduler 提交边界。ELF source 与缺页规则见[用户 ELF64 装载模块](user-elf.md)，任务与 MM 生命周期见[内核调度与进程生命周期模块](kernel-scheduler.md)。

## 入口与职责

| 文件 | 职责 |
|---|---|
| `kernel/exec.c`、`include/kernel/exec.h` | 捕获用户路径/argv/envp，打开可执行文件，解析并持有主程序和解释器 source，管理准备期 owner |
| `arch/riscv/exec.c`、`arch/riscv/elf_image.c` | 将 source 变成 Sv39 MM、入口 PC、SP 和架构线程状态 |
| `kernel/sched/exec.c` | 在不可返回提交点切换 `satp`、替换 MM、重建 Trap Frame 并处理 close-on-exec |
| `kernel/syscall/process.c`、`arch/riscv/trap.c` | 解码 syscall 221 和提交动作 |

通用入口 `kernel_execve_prepare()` 在旧 MM 上建立 `PREPARED` 事务；RISC-V `kernel_exec_image_prepare()` 调用 `riscv_elf_image_build()`。不使用运行期 vtable。LoongArch 后续实现同一 image 契约，但自行完成页表、布局和 trap 状态。

## 准备事务

准备顺序为：

```text
清理上次仍有 owner 的事务
-> 捕获 filename 并解析 cwd
-> 打开 regular executable OFD
-> 建立一次解析的主 ELF source
-> 若有 PT_INTERP，解析并打开唯一解释器 source
-> 捕获 argv/envp 内核快照
-> 构造新 MM、source-backed VMA、栈和 auxv
-> 发布 PREPARED
```

主程序支持 RISC-V `ET_EXEC` 和 `ET_DYN`；解释器支持非递归 `ET_EXEC`/`ET_DYN`。动态 source 的 `PT_DYNAMIC`/`PT_TLS` 等元数据留给用户动态链接器。主文件格式或架构错误映射为 `-ENOEXEC`；解释器路径不存在时保留路径 errno，解释器格式或架构不正确映射为 `-ELIBBAD`；资源、I/O 和参数限制分别返回 `-ENOMEM`、`-EIO`、`-E2BIG`。解析、打开或构造失败时旧进程映像完全不变。

事务中的 `executable_file`/`interpreter_file` 是 source 创建前的 OFD owners；source 创建成功后由 source 持有 OFD，事务改为持有 source owner。映像构造给 MM 增加各 source 的引用，事务在提交后释放自己的引用。解析、打开或真实 VFS/I/O 清理失败时保留准确 owner；物理页、VMA metadata 和堆的合法释放完成即结束，分配器不变量错误进入 fatal。

## 映像和初始状态

RISC-V 后端固定 Sv39/4 KiB，并支持：

- 非 PIE `ET_EXEC`，有或无 `PT_INTERP`；
- 带解释器的 PIE `ET_DYN`；
- 无解释器的 `ET_DYN`。

`PT_LOAD` 不在 exec 时复制整段文件，而登记 `ELF_PRIVATE` VMA。完整文件页使用 page cache+COW，文件/BSS 边界和 BSS 页按需建立私有页。缺页 I/O 在用户边界产生 `SIGBUS`，物理耗尽终止本次用户操作；新建可执行页完成 `SFENCE.VMA` 和必要 `FENCE.I` 后才可取指。source 的 program headers 只解析一次，避免重新读取造成 TOCTOU。

布局从 DTB `/chosen/rng-seed` 的可信 32 字节种子产生独立的 PIE/解释器 bias、mmap base、stack、brk 和 vDSO 随机量；没有种子时安全降级为确定性布局并省略 `AT_RANDOM`。初始 PC 是解释器入口（若有），`AT_ENTRY` 始终为主程序入口；初始栈保存真实 `AT_PHDR`、`AT_BASE`、`AT_ENTRY`、`AT_HWCAP`、`AT_RANDOM`（可用时）、`AT_EXECFN` 等 auxv。入口 SP 按 RISC-V psABI 16 字节对齐，scheduler 允许入口页尚未驻留，但必须属于可执行 VMA。

## 不可返回提交

提交前 scheduler 验证当前 task、新 MM 的 `satp`、入口可执行 VMA/PTE、栈 RW PTE 和 SP 对齐。线程组 exec 先终止其他成员并等待其调用栈与资源清理完成；非组长调用者接管原 TGID 和父子树位置。竞争 exec 的未提交事务由退出路径清理。随后：

```text
切换 new satp
-> 将 old MM 移入待清理 owner
-> 安装新 MM 和缓存 satp
-> 清零并重建 Trap Frame
-> 摘除 FD_CLOEXEC
-> 清理 old MM、source 和事务 owner
-> sret 到新入口
```

提交后不撤销新映像。TGID、cwd、files、fs context、`RLIMIT_NOFILE`/`RLIMIT_STACK` 和非 `FD_CLOEXEC` fd/OFD offset 保留；新栈布局按保留的软限制建立。非组长执行 exec 时调用线程接管原 TGID 作为 TID，旧 TID 释放。信号 disposition 按 exec 规则重置，F/D 状态清零。旧 MM 或 source 的真实 VFS/I/O 清理错误由持久 owner 继续处理，不改写新映像已经成功的结果。

## 验证与边界

```sh
make test-exec-riscv
make test-root-init-riscv
make test-riscv
```

动态 ET_DYN/解释器的生产构造已经接入；真实 userland runner 已验证动态 musl PIE、解释器、额外 DSO、初始 TLS 和运行中 dlopen TLS。musl/glibc 更广重定位矩阵、多线程 exec、shebang、`execveat`、凭据变化、写入文件的一致性和 LoongArch 后端仍未完成。128 KiB 是当前序列化初始栈镜像限制，不是完整 Linux `ARG_MAX` 策略。
