# RISC-V 根启动模块

本文描述生产内核从 QEMU 根盘启动 PID 1 的纵向生命周期。各层细节分别见[RISC-V VirtIO MMIO 块设备](riscv-virtio-block.md)、[VFS 与只读 ext4](vfs-ext4.md)和[用户 ELF64 装载](user-elf.md)。

## 启动路径

最终 Sv39、direct map、buddy 和 scheduler 就绪后，`arch/riscv/root_boot.c` 初始化页支持内核堆并为后续 exec 绑定同一物理分配器和内核根表，按 DTB 物理地址顺序选择首个成功初始化的 modern virtio-blk，raw whole-disk 只读挂载 ext4，并通过公共 executable-open 检查打开 `/init`。文件必须是 regular 且至少有一个 execute bit；请求使用 `argv[0]="/init"`、`argc=1`、`AT_EXECFN="/init"` 和空环境。

VFS 文件成为精确 `read_at` 源，ELF loader 把静态 RISC-V `ET_EXEC` 的 `PT_LOAD` 直接读入新 Sv39 用户页，随后关闭启动期 `/init` handle。根启动路径再创建借用根 mount、cwd 为 `/` 的 fs context 和空文件表，与 MM 一起原子转交 scheduler task。生产系统创建的第一个用户线程组得到 TID/TGID 1；任一步失败都由原 owner 按 files、fs、MM、用户页、mount、设备的依赖顺序清理。完成以上步骤后才启动 timer，因此任务不会在根对象尚未发布时运行。

没有 block device 时，生产内核保留无根 timer-idle 模式；一旦发现 block device，unsupported transport、挂载失败、缺失/不可执行 `/init` 或 ELF 错误都是明确的 root boot failure，不扫描后续磁盘寻找“碰巧可启动”的文件系统。

## PID 1、子进程与最终回收

PID 1 可以普通 clone 子进程并通过 wait4 回收。子进程退出时先释放 exec/files/fs/MM 重资源，再保留 PID、任务页和 wait status 成为 zombie；若清理失败则由 idle 从 exited 队列重试后再转 zombie。子进程继续派生的任务在父进程退出时重新挂到仍存活的 PID 1，因而不会因中间父进程消失而丢失可等待事件。

Completion 在任务对象与 PID 被释放前快照 TID/TGID。PID 1 自身退出时，尚存子进程会成为 parentless 并由 idle 静默清理；只有 PID 1 的全部文件资源、fs context、MM、TID 和任务页都已经释放，boot idle 才收到 PID 1 completion。复合清理失败保留准确 owner 阶段，不会提前卸载仍被 OFD/fs context 借用的根 mount。

当前没有用户空间重启或 init supervision，因此 PID 1 正常退出和故障都视为系统终止条件。根启动对象随后卸载 ext4、复位 VirtIO device、归还队列和堆页，并要求物理空闲页精确回到开始根启动前的基线、heap live/current pages 均为零，最后调用 SBI shutdown。

根 mount 在 PID 1 及其后代的文件资源回收期间保持存活。当前 `/init` 先通过 Linux RISC-V `openat/read/close` 读取根上的普通文件，覆盖绝对/相对路径、独立 offset、跨页大读取、fault 后 offset 保持、EOF 和错误 errno；随后从真实根盘依次 exec 相对路径 `stage2` 和绝对路径 `/stage3`。第三段映像执行普通 clone/wait：验证父子 MM 写隔离、继承 fd 的 OFD offset 共享、PPID、WNOHANG 与阻塞唤醒、进程组 selector、退出/故障 status、status EFAULT 后已回收，以及孙进程向 PID 1 reparent。PID 1 最终以状态 42 退出。

## 验证

```sh
make test-root-init-riscv
make test-exec-riscv
QEMU_MEMORY=1G make test-root-init-riscv
QEMU_MEMORY=16G make test-exec-riscv
make test-idle-riscv
```

fixture 是三个独立链接并写入真实 ext4 的静态 ELF，镜像还包含一个 9000 字节确定性数据文件、不可执行数据文件和可执行的非 ELF 脚本。程序在 U-mode 检查初始栈、errno、exec 与父子生命周期后以状态 42 调用 `exit(93)`。runner 要求 PID 1 身份、父子状态、fd/MM 语义、完整资源基线和 SBI 关机均成立；exec 聚焦入口还用链接器 wrapper 注入一次旧 MM 释放失败。无盘测试仍要求 timer idle 持续工作。
