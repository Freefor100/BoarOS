# RISC-V 根启动模块

本文描述生产内核从 QEMU 根盘启动 PID 1 的纵向生命周期。各层细节分别见[RISC-V VirtIO MMIO 块设备](riscv-virtio-block.md)、[VFS 与 ext4](vfs-ext4.md)和[用户 ELF64 装载](user-elf.md)。

## 启动路径

最终 Sv39、direct map、buddy 和 scheduler 就绪后，`arch/riscv/root_boot.c` 初始化页支持内核堆并为后续 exec 绑定同一物理分配器和内核根表，按 DTB 物理地址顺序初始化并登记所有 VirtIO MMIO version 1 legacy 或 version 2 modern virtio-blk，保留首个成功初始化的设备为根盘，按设备能力以读写或只读模式挂载 raw whole-disk ext4，并通过公共 executable-open 检查打开只读构建配置指定的 ELF。文件必须是 regular 且至少有一个 execute bit；默认请求使用 `argv[0]="/init"`、`argc=1`、`AT_EXECFN="/init"` 和空环境。

VFS 文件成为精确 `read_at` 源，ELF source 一次解析 RISC-V `ET_EXEC`/`ET_DYN` 的 program headers；根启动支持非递归 `PT_INTERP`，将 `PT_LOAD` 登记为专用 source-backed VMA，页面在首次取指或访问时按需物化。随机布局从 DTB `/chosen/rng-seed` 取得不计熵的初始材料；缺少种子时安全降级为确定性布局并省略 `AT_RANDOM`。根启动路径再创建借用根 mount、cwd 为 `/` 的 fs context 和空文件表，与 MM 一起原子转交 scheduler task。生产系统创建的第一个用户线程组得到 TID/TGID 1；生产 main 在根对象发布后启动页缓存 worker，再启动 timer，因此任务不会在根对象尚未发布时运行。worker 构造失败仍返回启动资源错误，不伪造后台回收成功。

生产 main 在 PID 1 和块设备 IRQ 就绪后可选启动 [VirtIO RNG worker](riscv-virtio-rng.md)，由 root 对象持有独立 IRQ、DMA 与 join handle。它在基线采样后分配，根结束/失败清理在检查基线前先 stop/join；只有确认设备 reset 才释放 DMA。无 RNG 或已完整回滚的启动失败不阻止 PID 1；reset 未确认则保留 owner 并明确报错。RNG 是内核 joinable 线程，不能成为用户 PID 1 completion。

未发布对象的失败清理由 `struct riscv_root_boot` 持久保存 file、原始 Sv39 space、MM、files、fs 与设备 owner，而不是留在 `riscv_root_boot_start()` 的栈帧中。真实 ext4/block I/O 清理若暂时失败，`start()` 返回 `RISCV_ROOT_BOOT_STATUS_CLEANUP` 且 root 进入 `RISCV_ROOT_BOOT_CLEANUP`；`riscv_root_boot_cleanup()` 只重试仍拥有的对象，生产 `kernel/main.c` 最多尝试三次，持续错误则报告并停止。物理页、VMA metadata 和堆的合法释放不产生 cleanup 状态，分配器不变量错误直接 fatal。

没有 block device 时，生产内核保留无根 timer-idle 模式；一旦发现 block device，unsupported transport、挂载失败、缺失/不可执行 `/init` 或 ELF 错误都是明确的 root boot failure，不扫描后续磁盘寻找“碰巧可启动”的文件系统。

## PID 1 stdio

创建 PID 1 文件表后，root boot 把 console 描述符绑定到 fd 0/1/2，使真实用户程序拥有 stdin/stdout/stderr。这是设备文件系统落地前的桥接：fork 继承描述符、exec 只摘除 CLOEXEC 槽，因此 stdio 跨整条进程链保持。console 的读写语义见[进程文件资源模块](kernel-files.md)。

## PID 1、子进程与最终回收

PID 1 可以普通 clone 子进程并通过 wait4 回收。子进程退出时先释放 exec/files/fs/MM 重资源，再保留 PID、任务页和 wait status 成为 zombie；若真实 VFS/block I/O 清理失败则由可调度 cleanup task 从 exited 队列按 owner 状态继续尝试，任务仍在自身栈上时任务页也会延后回收。子进程继续派生的任务在父进程退出时重新挂到仍存活的 PID 1，因而不会因中间父进程消失而丢失可等待事件。

Completion 在任务对象与 PID 被释放前快照 TID/TGID。PID 1 自身退出时，尚存子进程会成为 parentless 并由 cleanup task 静默清理；root cleanup task 保存 PID 1 completion，继续排空同轮 exited 队列中被重挂的孤儿 zombie，再检查根资源基线。否则 PID 1 恰好先于孤儿 zombie 被回收时会残留一个任务元数据页。复合清理失败保留准确 owner 阶段，不会提前卸载仍被 OFD/fs context 借用的根 mount；正常结束失败日志区分 ELF source、unmount、page cache、device、heap 与物理页基线。

当前没有用户空间重启或 init supervision，因此 PID 1 正常退出和故障都视为系统终止条件。根启动对象随后卸载 ext4、逆序注销并复位所有 VirtIO block device、归还各自队列和堆页，并要求物理空闲页精确回到开始根启动前的基线、heap live/current pages 均为零，最后调用 SBI shutdown。

根 mount 在 PID 1 及其后代的文件资源回收期间保持存活。当前 `/init` 先通过 Linux RISC-V `openat/read/close` 读取根上的普通文件，覆盖绝对/相对路径、独立 offset、跨页大读取、fault 后 offset 保持、EOF 和错误 errno；随后从真实根盘依次 exec 相对路径 `stage2` 和绝对路径 `/stage3`。三段映像分别验证 `brk` 初值、增长/缩小和 exec 重置；第三段还执行普通 clone/wait，验证精确 break 的父子独立性、shrink 后 heap 访问故障、重新增长零页、父子 MM 写隔离、继承 fd 的 OFD offset 共享、PPID、WNOHANG 与阻塞唤醒、进程组 selector、退出/故障 status、status EFAULT 后已回收，以及孙进程向 PID 1 reparent。PID 1 最终以状态 42 退出。

## 验证

```sh
make test-root-init-riscv
make test-exec-riscv
make test-root-boot-cleanup-riscv
QEMU_MEMORY=1G make test-root-init-riscv
QEMU_MEMORY=16G make test-exec-riscv
make test-idle-riscv
```

fixture 写入真实 ext4 的静态 ELF 以及 userland runner 使用的动态 musl PIE、解释器、额外 DSO 和 TLS；镜像还包含一个 9000 字节确定性数据文件、不可执行数据文件和可执行的非 ELF 脚本。程序在 U-mode 检查初始栈、errno、exec 与父子生命周期后以状态 42 调用 `exit(93)`。runner 要求 PID 1 身份、父子状态、fd/MM 语义、完整资源基线和 SBI 关机均成立。`make test-root-orphan-riscv` 让 PID 1 留下未等待的 zombie 子进程，验证 PID 1 completion 后继续排空退出队列。`test-root-boot-cleanup-riscv` 先让 fs context 创建失败，再在卸载日志时注入真实块写错误；三次清理调用必须保持相同 mount/cache/device owner、停止进一步写入，并最终报告 `CLEANUP`。关键日志错误不会因一次底层故障已消失就恢复为可写。无盘测试仍要求 timer idle 持续工作。

有块设备时，永久 cleanup task 在 root 基线快照前创建；root 启动仍处显式轮询阶段，调度用户前为每个 VirtIO block device 注册 DTB 提供的独立 PLIC 路由并切换运行期睡眠。退出清理、root finish/unmount 在该任务中执行；卸载前先停止并 join 页缓存 worker、释放其栈/快照/引用，再核对根资源基线，idle 只负责调度与 wfi；无盘启动不创建额外存储清理任务。跨高半区跳转不能继续使用寄存器中保存的旧物理栈指针，DTB 存储探测在独立 noinline 调用中完成。

## PID 1 构建配置

`make INIT_CONFIG=/absolute/profile.json` 指定 JSON 的 `path`、`argv`、`envp`；默认
`config/init.json` 为 `/init`、单参数和空环境。初始路径必须绝对，argv 至少一项；
NUL、路径/向量/字符串超限在构建期拒绝，最终初始栈仍由公共 ELF image 限制检查。
`AT_EXECFN` 来自 path，允许 argv[0] 与路径不同。入口错误明确报 root boot error，
不猜测备用路径；根启动要求 ELF，脚本由所选用户态解释器执行。

`tools/init-config.py` 每次构建检查配置，生成 build 目录内的只读 C 数据；内容不变
保持文件时间，root_boot.o 显式依赖生成文件。修改内容或切换回默认均重新验证依赖，
不会复用错误的启动配置。内核不包含环境创建或程序调度策略。

`make test-init-config-riscv` 在同一构建目录交替默认/自定义/默认/自定义配置，
真实 U-mode 验证 ELF 路径、不同 argv[0]、含空格参数、环境及 AT_EXECFN；缺失入口
必须失败。测试结束恢复默认配置。运行产物在 `build/init-config-run`，可安全重建。

## 块设备登记

根启动拥有设备对象直到 PID 1 完整收尾，每个对象独立持有队列、DMA、IRQ、
超时和统计。`riscv_root_boot_device(root, index)` 返回启动槽位；有效设备数由
`device_count` 给出，初始化失败按已取得的 owner 逆序释放，不把已初始化的
附加盘留在失败路径上。根盘内容无效仍直接失败，不尝试其他磁盘。

块设备号采用 Linux `new_encode_dev(252, index * 16)`，首盘为 `0xfc00`，
第二盘为 `0xfc10`；index 是 DTB 地址顺序中的成功 block device 序号。
无分区和热插拔，设备节点的设备号在本次启动内稳定。`kernel_block_lookup()`
借用根启动持有的设备生命周期；mount 通过 `kernel_block_claim()` 独占设备，
不同节点别名不能绕过同一设备的 claim。卸载成功后原 owner 必须调用
`kernel_block_release_claim()`；错误 owner 或重复释放是内部不变量错误。
有 claim 时注销返回 `EBUSY`，因此真实卸载错误仍保留设备 owner。

编码和 claim 语义依据固定 Linux
`references/linux/include/linux/kdev_t.h` 与 `references/linux/block/bdev.c`
（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）。BoarOS 不实现 Linux
分区或同 holder 的嵌套 claim，mount 仅持有一次独占 claim。

`make test-root-multi-block-riscv` 构建 `tests/userland/multi_mount.c` 的写入与
只读校验两个 musl ELF，在 legacy 和 modern 下各执行两次真实双盘启动。
写入阶段通过任意名字的 block 节点挂载第二盘，检查设备别名独占 claim、
裸盘 open 拒绝、独立盘内容、hardlink/nlink、嵌套 tmpfs、跨 mount EXDEV、
fd/mmap 阻止卸载、卸载重挂和共享映射写回。退出时保留第二盘 mount，要求
root finish 停止其 worker、同步日志和释放全部设备并精确恢复 heap/page 基线。
第二次启动把同一第二盘设为设备只读，读回包括 root finish 刷新的最后文件；
每次结束以 `e2fsck -fn` 检查第二盘。两种 transport 的四次启动均已通过。

生产启动使用 `riscv_root_boot_start_with_irq`，在 scheduler/PLIC/heap 就绪后、标准 OFD 打开前发布 DTB 串口 TTY。模块启动仍可通过原入口只保留早期 console。root 拥有 UART port 和 joinable worker，启动失败与正常退出都在最后 baseline 检查前停止并释放；硬件 drain 超时保留真实 port，失败标识为 `RISCV_ROOT_FINISH_UART`。具体队列、IRQ 与配置契约见[串口传输](riscv-uart-tty.md)。
