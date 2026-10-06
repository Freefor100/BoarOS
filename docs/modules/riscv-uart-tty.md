# 共用 ns16550 串口传输

`drivers/serial/ns16550.c` 和 `include/kernel/ns16550.h` 实现单 CPU 的 RX/TX、IRQ/worker、线路配置、drain 与 console 生命周期。RV 的 `arch/riscv/uart_tty.c` 仅提供 DTB/PLIC 适配；LA 的 `platform/loongarch_virt.c` 提供固定 QEMU UART 与 PCH-PIC/EIOINTC 适配。线路规程、read/write/ioctl/poll、作业控制和 OFD 生命周期由 `kernel_tty` 负责，本模块只提供实际硬件能力。

## 入口与 owner

root 的 heap 分配 port；port 保存 借用的 MMIO mapping、复制的 IRQ 回调和登记、joinable worker、1024 个字符/状态 RX 项和 1024 字节内核控制台队列。port 创建 TTY 后发布串口实例，标准 OFD 才能绑定它。`riscv_root_boot_start_with_irq` 是生产入口；原 `riscv_root_boot_start` 保留模块启动的早期 console。生产路径由 `kernel/main.c` 把固定早期 sink 与 DTB UART 的相交页合并后只映射一次，避免重复 PTE 冲突，然后初始化 PLIC，然后调用带 IRQ 的入口。

worker 使用调度器架构标准栈（RV 8 KiB、LA 32 KiB）和任务存储。root baseline 在创建 port/worker 前采集，正常 finish 必须先回收用户任务及其 OFD/session 引用，再 drain、停止和 join UART worker、注销 IRQ、销毁 TTY、释放 port，最后检查 heap 与物理页。启动失败路径先释放准备期 OFD，再停止 UART；没有遗失的 port 指针。TTY 销毁仍被真实引用阻塞或硬件 drain 超时会保留 port，下一次 stop 可继续进展；合法 allocator 释放失败直接 fatal，不转换成清理重试。

## IRQ、worker 与线路

- 每次 UART handler 最多处理 64 个硬件字符。字符及 OE/PE/FE/BI 状态来自实际 LSR；IRQ 不分配、不睡眠、不调用线路规程。满 RX 环关闭 RDI/RLSI，worker 消费后恢复；硬件 OE 计入真实 overrun，满软件队列本身不伪造硬件错误。
- worker 每轮最多提交 256 个 RX 项，发送内核 console 与 TTY 用户/echo 合计最多 256 字节，轮换先服务的来源。仍有可执行工作时 yield；THRE credit 缺失时启用发送就绪 IRQ，handler 关闭该中断并唤醒 worker。THRE 只证明可继续发送，drain 同时要求软件队列为空与实际 TEMT；最后的 TEMT 以 10 ms 请求期限再次观察，避免空转 IRQ；调度可能推迟实际恢复，不是硬上界。
- `transmit` 逐次检查 THRE，返回真实接受字节；没有分配、睡眠或成功存根。default 115200/8N1/CREAD 的 divisor 来自 DTB 时钟；CS5–8、parity、stop bits 实际写 LCR。B0 撤 DTR/RTS；HUPCL 在 last close 撤 modem 输出，后续 configure 可恢复。独立输入速度归一到硬件输出速度，未实现的 CMSPAR 和 CRTSCTS 不对用户保留支持声明。
- `virt_uart_putc` 先把真实内核字符保存在 16 KiB klog，再按 console level 送到独立 port 队列。`kernel_console_putc` 是不写 klog 的诊断输出；TTY 用户/echo 不经过这两条路径。queue 满时记录 `console_dropped`，IRQ 打印不阻塞。`virt_uart_emergency_begin` 使 fatal 输出改用既有固定轮询 sink；pre/post runtime 也保持轮询输出。
- stop 首先阻止新 TTY 工作并关闭输入，在正常关闭中保留已接受且未 flush 的 TX。超过一秒仍无法 drain 返回 EIO，worker 已 join，但队列、MMIO 和 IRQ 登记仍有 port owner；再次 stop 可重建 worker 重试。成功停止在 join 后注销 IRQ，才释放 TTY/port。`stop_report` 在 drain/join 完成后、释放前冻结最终计数；失败或空 owner 不修改输出，root 保存该固定快照供聚焦验证。

fatal入口先关闭中断并选择同步sink，绕过日志级别和worker队列；物理页释放诊断也
在打印元数据前选择该入口。普通内核日志仍按原环与级别处理。宿主反例证明旧路由
在console关闭时丢失fatal字符，以及分配器诊断未选择同步sink；修复后硬件输出和
allocator fatal原因均保留。该检查保护新UART接入，不反推历史fatal的唯一触发链。

准备阶段在单hart关中断区间内创建core、登记IRQ和worker，最后才发布owner/TTY。
仅在这个明确的setup rollback状态，软件对象尚未接受TX、没有可见客户，PIO硬件里
早期raw的尾字节也不借用port/core内存：撤销IRQ、stop/join可能已有worker后，可以
释放未发布的软件owner而不等待该尾字节。回调断言无接受的TX、无队列/登记/worker；
这不放宽已发布port的tcdrain/stop TEMT承诺。正常启动从初始LSR记录尚忙的硬件尾字节，
即使第一轮没有新TX也保留10ms观察期限，TEMT变空后通知真实core的drain等待者。

RV64/COST关闭构建的DWARF对象大小为port **3296 B**、core **29032 B**。真实create模型
核对了这两个分配请求；当前heap的大对象按请求页数上取整为buddy order，分别占1与8个
4KiB页，合计9个heap页（36KiB）。worker另有标准8KiB物理栈（2页，含16B guard/canary区域）
和2368B任务metadata（独立1页），固定直接owner合计12页（48KiB），不包含后续OFD实例、
用户任务、session/PID引用或其他heap owner。此次编译worker自身栈帧608B，仍需与调用链及
运行高水位分开解释，不以单个`.su`代替完整运行栈边界。

## 验证

```sh
make test-uart-host
make build/riscv/arch/riscv/uart_tty.o
```

宿主 fixture 使用生产传输实现与真实 IIR/LSR/IER 行为模型，TTY 边界只模拟回调；保护 IRQ 预算、满环/恢复和索引回绕、LSR 错误位、控制台 overflow、部分发送、发送 credit IRQ、TEMT 期限、modem/configure、四类启动失败与 unregister，以及关闭超时 owner 保留/恢复。新增fixture链接实际 `fs/tty.c`，分别保护TEMT忙时IRQ/worker启动失败的rollback、第一空worker期限/真实drain唤醒和正常stop超时保留/恢复；准备状态的公开入口不可接受TX，模型同时检查IRQ临界区。ASAN/UBSAN 验证 owner 清理。DTB fixture 另验证地址/clock/route/layout 与失败保持输出。真实 U-mode 和整合回归由集成 owner 验证，宿主 fixture 或 object 编译不等于真实 TTY ABI 已通过。

固定资料：`references/qemu/hw/riscv/virt.c` 与 `hw/char/serial.c`，QEMU v11.1.0 commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`；`references/linux/drivers/tty/serial/8250/8250_port.c`，Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。本实现没有复制外部代码。本传输范围不包含 modem 输入查询、硬件流控、多串口、DMA 或实板验证；
PTY 使用独立软件 transport，见[TTY 模块](kernel-tty.md)。

## termios2 与线路速度

44 字节 termios2 支持标准速率与 BOTHER 数字速率。UART 使用同一个 RX/TX 时钟，
将输入归一到输出；TTY core 保存回读速度，旧 36 字节 ioctl 不越界访问附加字段。
设备设置先校验速度范围和 16 位 divisor，再在短发布区写 DLL/DLH、LCR、MCR、IER；
失败保留旧硬件与软件状态。B0 保留原 divisor、撤 modem 输出并关闭 RX。

ABI 报告的是接受的名义速度，硬件通过 DTB 时钟及四舍五入的 divisor 实现近似线路
速度，两者不是同一个测量值；固定 Linux 8250 也区分这两个边界。本驱动在 termios2
硬件范围之外返回 EINVAL，不承诺完整复制 Linux 的超范围回退策略。
CMSPAR/CRTSCTS 沿现有支持集合归一，不伪装硬件流控已经实现。

`make test-uart-host` 核对真实寄存器别名、divisor、线路格式、速度归一、B0 和
错误不发布；`make test-tty-termios2-riscv` 用同一 ELF 在真实串口核对
44 字节复制、标准/数字速度、坏指针、drain/flush 与旧 36 字节布局。
探针先明确设置双方共有的线路基线，最后恢复各自原配置，不将不同启动默认值当作 ABI 差异。

## LA 平台与对照验收

固定 QEMU `references/qemu/hw/loongarch/virt.c`、`include/hw/loongarch/virt.h`
(commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`) 的第一 UART 为
`0x1fe001e0/0x100`，字节寄存器、shift=0、clock=1843200 Hz、PCH pin2。
该信息仅在平台层构造；共用核心不包含 RV CSR 或 PLIC 地址。IRQ callbacks 按值复制，
mapping 由 root/platform 持有至 stop 成功。LA root 在创建标准 OFD 前发布 TTY，
在用户/OFD/session owner 释放后 drain/join、注销 IRQ、销毁 core/port，再检查完整基线。
正常 LA 内核日志进入同一 klog 环并遵守 console level；早期/退出后的 console 用轮询，
fatal 先关中断并绕过日志级别与拥塞队列。

`tests/tty/riscv.py --arch loongarch` 共用原始串口字节通道、输入和结果解析。
`tests/tty/pty_riscv.py --arch loongarch` 共用 PTY 案例、原版 BusyBox、
原版 glibc API 和磁盘真实重启检查。每架构内固定同一 ELF/fixture，
Linux 先运行、BoarOS 后运行；LA 默认分别执行512 MiB/1 GiB。
Linux PTY coordinator 作为真实 PID1 验证原 script 的收养/reap，退出42由 Linux panic
报告实际 wait status；这不是 Linux 资源基线证据。BoarOS 根 owner 必须全部回收。

入口：`make test-tty-diff-loongarch test-tty-termios2-loongarch test-tty-loongarch
test-pty-loongarch test-pty-apps-loongarch test-uart-failures-loongarch`。
共同串口27、作业控制80、termios2 29条记录一致；PTY core9、musl/GNU API2、
原 script21条及重启1条一致。原 ash/stty 交互覆盖Ctrl-C/Z、bg/fg、TTIN/TTOU和回收。
UART port/core heap、worker任务页/栈、IRQ共五类构造失败不发布PID1，
两种RAM回到页/堆/栈/BAR基线；另有活跃UART+console关闭+满队列的fatal轮询证据。
宿主真实core模型保护超时保留owner与后续stop、THRE/TEMT和启动回滚。
RTC和完整指定程序矩阵在本阶段尚未验收，不由终端通过推导能力全部对齐。
