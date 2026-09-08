# 进程映像替换模块

本文描述当前 Linux `execve(221)` 的通用准备事务、RISC-V 映像后端和 scheduler 提交边界。ELF 物化规则见[用户 ELF64 装载模块](user-elf.md)，用户输入见[用户内存访问模块](kernel-uaccess.md)，任务与 MM 生命周期见[内核任务调度模块](kernel-scheduler.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `include/kernel/exec.h`、`kernel/exec.c` | 从 current task 取得 files/fs/MM，以可重试事务准备新映像 |
| `include/kernel/exec_image.h` | 定义构建期选择的架构映像请求和结果，不暴露 Sv39 类型 |
| `include/arch/riscv/exec.h`、`arch/riscv/exec.c` | 绑定 RISC-V 分配器与内核根表，把静态 ELF 装载结果变成通用 MM |
| `kernel/sched/exec.c` | 验证 prepared 事务、切换 `satp`、替换 MM 和 Trap Frame、处理 close-on-exec |
| `kernel/syscall/process.c`、`arch/riscv/trap.c` | 解码 Linux syscall 221，并区分返回旧程序与进入新程序两种动作 |

用户接口为 Linux RISC-V `execve(const char *filename, char *const argv[], char *const envp[])`，成功不返回；当前只允许单成员线程组执行。内部 `kernel_execve_prepare()` 在成功时只建立 `PREPARED` 事务，Trap 边界随后调用 `kernel_scheduler_exec_commit()` 完成不可返回的提交。

通用 exec 层与架构映像层在构建期连接，不使用运行期 vtable。通用层拥有路径、用户字符串快照、已打开可执行文件和事务生命周期；RISC-V 后端负责 ELF/Sv39、入口、SP、`satp` 可用性与架构私有清理。LoongArch 后续可以实现同一 `kernel_exec_image_prepare/cleanup` 契约，而不复用 Sv39 结构。

## 准备顺序与用户可见错误

准备阶段借用旧 MM，并按以下顺序执行：

```text
清理上次待重试事务
-> 复制 filename（最多 4096 字节）并按 cwd 解析
-> 打开并检查可执行普通文件
-> 一次性捕获 argv/envp 字符串
-> 构造新地址空间、入口和初始栈
-> 登记新映像的 ELF/栈 VMA
-> 发布 PREPARED 事务
```

可执行文件先于参数向量读取，因此不存在的文件即使配合坏 `argv` 也先返回 `-ENOENT`。空路径返回 `-ENOENT`，不可读用户地址返回 `-EFAULT`，路径无 NUL 返回 `-ENAMETOOLONG`，目录、非普通文件或没有任何执行位返回 `-EACCES`。当前静态 ELF 格式、架构或布局不支持时返回 `-ENOEXEC`；分配失败返回 `-ENOMEM`，参数/环境字符串、指针表、auxv、`AT_EXECFN` 和对齐合计超过 128 KiB 返回 `-E2BIG`。

`argv==NULL` 或首项为 NULL 会规范化成一个空的 `argv[0]`；`envp==NULL` 表示空环境。参数向量和每条字符串都按用户页逐段复制，任何用户指针只读取一次进入内核所有的快照；之后旧地址空间可以在提交时安全替换。当前单 hart、单成员线程组不会在快照期间并发执行 `unmap/mprotect`；引入共享 MM、SMP 或 COW 后，这个过程必须纳入 MM 读侧稳定协议。

新地址空间转入 MM 后，RISC-V 后端在 executable file 仍由事务持有时登记按页权限合并的匿名 ELF VMA 和完整栈 reserve VMA；因此后续缺页或 VMA 操作有逻辑区间，而不是只观察已驻留 PTE。普通 Linux 失败保持当前 MM、寄存器、PID/TID、cwd、文件表和所有 fd 不变，并从原 `ECALL` 的下一条指令返回负 errno。已分配的新映像和临时缓冲区由附着在 task 上的事务清理；VMA metadata、物理页或其他资源释放暂时失败时 owner 仍保存在事务中，由下次 exec 或退出 reaper 重试。

## 提交点与永久状态

提交前 scheduler 重新检查 current task、单成员线程组、新 MM、可执行入口映射、可读写栈映射和 16 字节 SP 对齐，并取得有效 `satp`。这些检查全部通过后才进入不可返回区：

```text
switch to new satp
-> move old MM into retired owner（vfork 共享引用在此点释放，唤醒父进程）
-> install new MM and cached satp
-> zero and rebuild complete user Trap Frame
-> detach FD_CLOEXEC descriptors
-> retire old MM and transaction allocations
-> sret to new entry
```

提交保持 task、TID/TGID、files、fs context 和 cwd 对象不变，所以非 `FD_CLOEXEC` fd 及其 open-file offset 跨 exec 保留。被标记的 fd 在提交后先从表中逻辑摘除；即使 VFS close 或堆释放需要重试，新程序也只能看到 `EBADF`。新 Trap Frame 除入口 PC、SP、架构给出的 `tp`、用户返回所需 `sstatus` 和可信 `kernel_tp` 外全部清零，旧程序的参数寄存器、callee-saved 寄存器和 syscall 返回现场不会泄漏到新映像。

`satp` 切换是 point of no return。之后旧用户页不能再安全恢复，因而任何意外内部状态错误都会终止当前任务并交给 reaper，不能向旧程序伪造一次普通失败返回。可重试的资源清理失败不撤销已提交映像；事务继续挂在 task 上，后续 exec 或退出路径收口。

## 验证与限制

```sh
make test-user-elf-cases-riscv
make test-files-riscv
make test-syscall-riscv
make test-scheduler-riscv
make test-exec-riscv
QEMU_MEMORY=16G make test-exec-riscv
make test-riscv
```

生产测试把独立链接的 `/init`、`/stage2` 和 `/stage3` 写入真实只读 ext4，实际进入 U-mode 后连续执行相对和绝对路径 exec。测试覆盖路径/ELF/argv/envp/大小错误、失败原子性、PID/TID/cwd/fd offset 保持、close-on-exec、`AT_EXECFN`、寄存器重置、空 argv/envp 规范化、最终资源基线，并注入旧 MM 首次释放失败验证提交后的可重试清理。

当前仅支持 RISC-V64 静态 `ET_EXEC`、单 hart 和单成员线程组。exec 已实现标准信号 disposition 重置（自定义 handler 恢复为 DFL、IGN 保留）以及 F/D 状态清零；没有 shebang、`ET_DYN`/PIE、动态解释器、TLS、多线程 exec 收拢、凭据变化、`execveat`、文件写入并发规则或 LoongArch 映像后端。128 KiB 是当前整个初始栈映像的硬上限，不是完整 Linux `ARG_MAX` 实现。
