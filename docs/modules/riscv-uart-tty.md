# RISC-V 串口传输

`arch/riscv/uart_tty.c` 实现单 hart 上一个 DTB 发现的 ns16550 串口传输；`include/arch/riscv/uart_tty.h` 是 root owner 接口。线路规程、read/write/ioctl/poll、作业控制和 OFD 生命周期由 `kernel_tty` 负责，本模块只提供实际硬件能力。

## 入口与 owner

root 的 heap 分配 port；port 保存 MMIO mapping、PLIC 登记、joinable worker、1024 个字符/状态 RX 项和 1024 字节内核控制台队列。port 创建 TTY 后发布串口实例，标准 OFD 才能绑定它。`riscv_root_boot_start_with_irq` 是生产入口；原 `riscv_root_boot_start` 保留模块启动的早期 console。生产路径由 `kernel/main.c` 把固定早期 sink 与 DTB UART 的相交页合并后只映射一次，避免重复 PTE 冲突，然后初始化 PLIC，然后调用带 IRQ 的入口。

worker 使用调度器标准 8 KiB 栈和任务存储。root baseline 在创建 port/worker 前采集，正常 finish 必须先回收用户任务及其 OFD/session 引用，再 drain、停止和 join UART worker、注销 IRQ、销毁 TTY、释放 port，最后检查 heap 与物理页。启动失败路径先释放准备期 OFD，再停止 UART；没有遗失的 port 指针。TTY 销毁仍被真实引用阻塞或硬件 drain 超时会保留 port，下一次 stop 可继续进展；合法 allocator 释放失败直接 fatal，不转换成清理重试。

## IRQ、worker 与线路

- 每次 UART handler 最多处理 64 个硬件字符。字符及 OE/PE/FE/BI 状态来自实际 LSR；IRQ 不分配、不睡眠、不调用线路规程。满 RX 环关闭 RDI/RLSI，worker 消费后恢复；硬件 OE 计入真实 overrun，满软件队列本身不伪造硬件错误。
- worker 每轮最多提交 256 个 RX 项，发送内核 console 与 TTY 用户/echo 合计最多 256 字节，轮换先服务的来源。仍有可执行工作时 yield；THRE credit 缺失时启用发送就绪 IRQ，handler 关闭该中断并唤醒 worker。THRE 只证明可继续发送，drain 同时要求软件队列为空与实际 TEMT；最后的 TEMT 由最多 10 ms 的定时等待观察，避免空转 IRQ。
- `transmit` 逐次检查 THRE，返回真实接受字节；没有分配、睡眠或成功存根。default 115200/8N1/CREAD 的 divisor 来自 DTB 时钟；CS5–8、parity、stop bits 实际写 LCR。B0 撤 DTR/RTS；HUPCL 在 last close 撤 modem 输出，后续 configure 可恢复。独立输入速度归一到硬件输出速度，未实现的 CMSPAR、BOTHER 和 CRTSCTS 不对用户保留支持声明。
- `virt_uart_putc` 先把真实内核字符保存在 16 KiB klog，再按 console level 送到独立 port 队列。`kernel_console_putc` 是不写 klog 的诊断输出；TTY 用户/echo 不经过这两条路径。queue 满时记录 `console_dropped`，IRQ 打印不阻塞。`virt_uart_emergency_begin` 使 fatal 输出改用既有固定轮询 sink；pre/post runtime 也保持轮询输出。
- stop 首先阻止新 TTY 工作并关闭输入，在正常关闭中保留已接受且未 flush 的 TX。超过一秒仍无法 drain 返回 EIO，worker 已 join，但队列、MMIO 和 IRQ 登记仍有 port owner；再次 stop 可重建 worker 重试。成功停止在 join 后注销 IRQ，才释放 TTY/port。`stop_report` 在 drain/join 完成后、释放前冻结最终计数；失败或空 owner 不修改输出，root 保存该固定快照供聚焦验证。

fatal入口先关闭中断并选择同步sink，绕过日志级别和worker队列；物理页释放诊断也
在打印元数据前选择该入口。普通内核日志仍按原环与级别处理。宿主反例证明旧路由
在console关闭时丢失fatal字符，以及分配器诊断未选择同步sink；修复后硬件输出和
allocator fatal原因均保留。该检查保护新UART接入，不反推历史fatal的唯一触发链。

## 验证

```sh
make test-uart-host
make build/riscv/arch/riscv/uart_tty.o
```

宿主 fixture 使用生产传输实现与真实 IIR/LSR/IER 行为模型，TTY 边界只模拟回调；保护 IRQ 预算、满环/恢复和索引回绕、LSR 错误位、控制台 overflow、部分发送、发送 credit IRQ、TEMT 期限、modem/configure、四类启动失败与 unregister，以及关闭超时 owner 保留/恢复。ASAN/UBSAN 验证 owner 清理。DTB fixture 另验证地址/clock/route/layout 与失败保持输出。真实 U-mode 和整合回归由集成 owner 验证，宿主 fixture 或 object 编译不等于真实 TTY ABI 已通过。

固定资料：`references/qemu/hw/riscv/virt.c` 与 `hw/char/serial.c`，QEMU v11.1.0 commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`；`references/linux/drivers/tty/serial/8250/8250_port.c`，Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。本实现没有复制外部代码。当前范围不包含 modem 输入查询、硬件流控、多串口、DMA、PTY 或实板验证。
