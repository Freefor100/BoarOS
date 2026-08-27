# RISC-V 根启动模块

本文描述生产内核从 QEMU 根盘启动 PID 1 的纵向生命周期。各层细节分别见[RISC-V VirtIO MMIO 块设备](riscv-virtio-block.md)、[VFS 与只读 ext4](vfs-ext4.md)和[用户 ELF64 装载](user-elf.md)。

## 启动路径

最终 Sv39、direct map、buddy 和 scheduler 就绪后，`arch/riscv/root_boot.c` 初始化页支持内核堆，按 DTB 物理地址顺序选择首个成功初始化的 modern virtio-blk，raw whole-disk 只读挂载 ext4，并打开 `/init`。文件必须是 regular 且至少有一个 execute bit；请求使用 `argv[0]="/init"`、`argc=1` 和空环境。

VFS 文件成为精确 `read_at` 源，ELF loader 把静态 RISC-V `ET_EXEC` 的 `PT_LOAD` 直接读入新 Sv39 用户页，随后关闭 `/init`，把地址空间依次 move 到 MM 和 scheduler task。生产系统创建的第一个用户线程组得到 TID/TGID 1。完成以上步骤后才启动 timer，因此任务不会在根对象尚未发布时运行。

没有 block device 时，生产内核保留无根 timer-idle 模式；一旦发现 block device，unsupported transport、挂载失败、缺失/不可执行 `/init` 或 ELF 错误都是明确的 root boot failure，不扫描后续磁盘寻找“碰巧可启动”的文件系统。

## PID 1 退出和回收

用户退出或故障先进入 scheduler exited 队列。Completion 在任务对象与 PID 被释放前快照 TID/TGID，并允许 reaper 在 MM、PID 或任务页释放失败后从准确阶段重试。只有 reaper 已释放 PID 1 的地址空间、TID 和任务页，boot idle 才处理 PID 1 completion。

当前没有用户空间重启或 init supervision，因此 PID 1 正常退出和故障都视为系统终止条件。根启动对象随后卸载 ext4、复位 VirtIO device、归还队列和堆页，并要求物理空闲页精确回到开始根启动前的基线、heap live/current pages 均为零，最后调用 SBI shutdown。

根 mount 在 PID 1 运行期间保持存活，为后续文件 syscall 保留正确演进方向；当前 `/init` 本身只测试启动 ABI 与 `exit(93)`，尚无 fd table、动态链接器、fork/exec/wait 或长期用户空间。

## 验证

```sh
make test-root-init-riscv
QEMU_MEMORY=1G make test-root-init-riscv
QEMU_MEMORY=16G make test-root-init-riscv
make test-idle-riscv
```

fixture 是独立链接并写入真实 ext4 的静态 ELF。它在 U-mode 检查栈对齐、`argc=1`、`argv[0]` 和空 envp，再以状态 42 调用 Linux RISC-V `exit(93)`。runner 要求 PID 1 身份、状态、完整资源基线和 SBI 关机均成立；无盘测试则要求 timer idle 持续工作。
