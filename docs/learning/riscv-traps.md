# RISC-V Trap 学习总结

本文整理实现 RISC-V S/U-mode trap、异常返回和中断入口需要掌握的知识，以及 BoarOS 当前已经确定的选择。当前代码接口和限制以 [RISC-V Trap 模块](../modules/riscv-trap.md) 为准。

## Trap 不是普通函数调用

Trap 是处理器因同步异常或异步中断改变控制流的统称。普通函数调用由软件按 ABI 主动执行，调用者知道哪些寄存器可能被破坏；trap 可以出现在任意指令边界，被打断代码不能提前把现场整理成函数调用形式。因此入口汇编必须先保存后续处理会破坏、而返回时仍需要的架构状态，才能安全调用 C。

同步异常由当前指令直接导致，例如非法指令、`EBREAK`、未对齐访问和页故障。异步中断来自指令流之外，例如软件、定时器或外部设备；它与被打断指令没有“故障指令需要跳过”的关系。这一区别直接决定 `sepc` 的处理方式。

## 硬件进入 S-mode 时保存了什么？

当 trap 被委托或送入 S-mode 时，处理器完成最小硬件状态迁移：

- `sepc` 保存被中断或发生异常的指令虚拟地址。
- `scause` 最高位表示 interrupt，其余位给出 cause code。
- `stval` 保存异常特有的辅助信息；页故障通常给出故障虚拟地址，但部分异常允许为零，不能把非零作为通用不变量。
- `sstatus.SPP` 记录 trap 前的特权级：0 表示 U-mode，1 表示 S-mode。
- 原 `sstatus.SIE` 被复制到 `SPIE`，随后 SIE 被硬件清零。
- PC 转向 `stvec` 指定的入口。

硬件不会自动保存 x1..x31，也不会替软件选择内核栈。若入口在保存 `t0` 前用它读取 CSR，原 `t0` 就已经丢失；若 trap 来自 U-mode 而入口继续使用用户 `sp`，内核甚至无法信任 Frame 将写到哪里。这就是 trap 入口必须由少量严格排序的汇编承担的原因。

## `stvec` 的 Direct 和 Vectored 模式

`stvec` 同时保存入口 BASE 和 MODE：

- Direct 模式下，所有异常和中断都进入 BASE，由公共入口读取 `scause` 后分类。
- Vectored 模式下，同步异常仍进入 BASE，异步中断进入 `BASE + 4 × cause`，各槽通常再跳到具体汇编桩或公共保存路径。

Vectored 模式最多减少少量 cause 分类工作，不能免除需要抢占或调度时的上下文保存。BoarOS 当前没有高频正式中断 handler，选择 Direct 公共入口可以让保存、验证和错误策略只有一份。若以后测量证明入口分类成为瓶颈，再增加向量快速路径不会改变 Trap Frame ABI。

## `sstatus` 怎样保护进入和返回窗口？

SIE 是 S-mode 全局中断开关，`sie` 则分别控制 supervisor software、timer、external 等中断源。一个 S-mode 中断只有在对应 `sip` pending、`sie` enable 且全局条件满足时才会被接收。

进入 trap 时，硬件把旧 SIE 压入 SPIE 并清 SIE，使入口能够原子地保存关键现场。BoarOS 当前在整个 dispatcher 期间保持 SIE 为零，不允许异步中断嵌套。这样延迟取决于 handler 最长执行时间，因此以后的中断代码应保持硬中断部分很短，把耗时工作推迟到普通内核上下文，而不是轻率地在 Frame 尚未稳定时重新开中断。

`sret` 完成相反转换：PC 取自 `sepc`，返回特权级取自 SPP，SIE 取自 SPIE，随后 SPIE 置一、SPP 清零。BoarOS 接受 S-mode Frame，也接受通过可信内核任务指针验证的 U-mode Frame；两者都拒绝保存 SIE 为一，防止写回 `sstatus` 后、通用寄存器恢复前出现新的中断。首次进入 U-mode 时还显式设置 `UXL=64`、`SPIE=1`，并保持 `SPP=0`、`SIE=0`。

## 同步异常和异步中断怎样继续执行？

对 `ECALL`、`EBREAK` 和页故障等同步异常，`sepc` 指向异常指令自身。如果 handler 决定消费该事件，必须根据指令语义选择重试、终止当前任务或前移 `sepc`。`ECALL` 完成后应越过调用指令；demand paging 补好 PTE 后必须保持 `sepc` 不变，让同一条 load/store/fetch 重试。RISC-V 支持 16 位压缩指令后，不能对所有 breakpoint 都盲目增加 4；BoarOS 的恢复测试显式编码 32 位 `EBREAK`，所以测试 handler 的 `sepc += 4` 有确定依据。生产内核当前不把任意 breakpoint 当作可恢复事件。

RISC-V 把 instruction、load、store/AMO page fault 分成 cause 12、13、15，`stval` 给出故障虚拟地址。cause 说明本次访问类型，VMA 权限决定它是否逻辑允许；不能只看到“VMA 存在”就补页。若 PTE 更新发生在当前地址空间，返回前还必须按平台并发模型执行 `SFENCE.VMA`：BoarOS 当前是 ASID 0、单 hart，因此成功补页后做本地单 VA 失效；SMP 共享 MM 需要远端 shootdown。

异步中断的 `sepc` 表示恢复执行的位置，一般不需要前移。handler 必须先解除中断条件：软件中断可以清 `sip.SSIP`，定时器中断需要把比较值安排到未来或关闭对应 source，外部中断需要经平台中断控制器 claim/complete。若 pending 状态不消失，`sret` 恢复 SIE 后会立即再次 trap，形成中断风暴。

## 为什么保存完整整数现场？

中断发生时，x1..x31 都可能承载被打断代码仍需使用的值。只考虑 C 函数的 caller-saved/callee-saved 分类可以构造较小的即时返回入口，但无法直接作为抢占、调度、信号和用户态异常的完整执行上下文。BoarOS 保存全部可写整数寄存器和四个 trap CSR，并在原有尾槽保存入口取得的可信内核 `tp`，得到固定 288 字节、16 字节对齐的 Frame。

基础内核按 RV64IMAC 构建，因此整数入口不保存 F/V 状态；用户 F/D 状态由独立的 272 字节 per-task image 依据 `sstatus.FS` 按需保存恢复。这样不会把 FP 或未来更大的向量上下文塞进每次基础 trap。当前只实现 F/D，V 扩展仍需要独立的 owner、异常和调度协议。

S-mode trap 直接在当前内核栈建立 Frame；U-mode trap 先从 current 取得可信内核栈，再建立相同布局的 Frame。两种路径都继续消耗所属任务内核栈的剩余空间；当前 boot idle 使用 16 KiB 静态栈，普通内核/用户任务使用独立 8 KiB 连续物理栈，没有 guard page 或溢出恢复。是否再设置 per-hart IRQ 栈应根据嵌套、中断负载和栈高水位决定。Trap Frame 与 scheduler switch context 的分工见[内核线程与抢占调度学习总结](kernel-scheduling.md)。

## `sscratch` 与 U-mode 换栈

`sscratch` 是留给 S-mode 软件使用的 XLEN 位 CSR。规范建议在运行用户代码时保存 hart-local supervisor context 指针，并在入口最前面通过 `CSRRW` 与某个整数寄存器交换，从而在还没有可信内核栈时取得内核上下文。

Linux RISC-V 在用户态运行时让 `sscratch` 指向内核的当前任务/线程上下文；trap 入口交换 `tp`，再从该上下文取得 kernel SP。内核态执行期间则把 `sscratch` 置零，使递归 trap 能识别它已经来自内核。

BoarOS 采用同一约定。运行 U-mode 时，`sscratch` 保存 current，`tp` 是用户寄存器；入口第一步用 `csrrw tp, sscratch, tp` 取得 current 并暂存用户 `tp`，把不可信用户 `sp` 写入任务控制块，再从控制块加载 kernel SP。完整 Frame 建好后把用户 `sp/tp` 放回对应槽、把可信 current 写入 `kernel_tp` 槽，并在调用 C 前清零 `sscratch`。用户 `gp` 同样不可信；保存它以后必须以禁止 linker relaxation 的 PC 相对序列重载内核 `__global_pointer$`，避免 C 的全局访问使用用户选择的基址。

从 S-mode 进入时 `sscratch=0`，第一次交换得到零，入口据此留在当前栈，再交换一次恢复原内核 `tp`。返回 U-mode 前必须验证 Frame 中的 `kernel_tp` 与当前内核 `tp` 一致，然后把 current 放回 `sscratch` 并恢复用户 `tp/sp`。可信 `kernel_tp` 由入口而不是用户寄存器产生，因此返回代码不必把任意用户值当作内核指针解引用。

## 为什么返回前清除 LR/SC reservation？

原子扩展的 `LR` 会在 hart 内建立 reservation，后续 `SC` 根据 reservation 是否仍成立决定成功。Trap Frame 无法有意义地保存并恢复这一微架构状态；handler 或调度切换还可能执行另一组 LR/SC。

规范允许 `SRET` 清 reservation，但不要求所有实现都清。Linux 因此在返回前执行一次指向可写 Frame 槽的 dummy `SC`。BoarOS 采用同样的原则：对保存 `sepc` 的栈槽执行 `sc.d`，源值就是槽内原值，所以即使条件存储成功也不改变 Frame，同时显式结束旧 reservation。

## 当前项目选择

BoarOS 当前固定以下 RISC-V trap 基线：

- Direct `stvec` 和一条公共保存/恢复路径。
- S-mode 使用当前内核栈；U-mode 通过 `sscratch <-> tp` 切到任务内核栈，并复用同一完整 RV64 整数 Trap Frame。
- 汇编只负责架构现场和关键返回验证，具体 cause 交给 C dispatcher。
- handler 返回表示事件已经处理；未知或当前不能处理的事件必须 fatal，不伪造成功。
- dispatcher 期间保持 SIE 关闭，不支持嵌套异步中断。
- 最终地址空间稳定后开启 supervisor timer interrupt；生产 dispatcher 处理 timer、U-mode ecall、U-mode 同步故障以及公共用户返回尾的标准 signal handler/sigreturn 和 syscall restart。cause 12/13/15 先尝试依据当前 MM 的 VMA fault policy 补页，成功时重试原指令；权限/范围错误只终止所属任务，匿名补页 OOM 以资源原因终止，页表状态或清理失败仍 fatal。信号 handler 通过固定 RX VDSO ecall stub 返回，dispatcher 不提供超出当前标准子集的运行期架构注册框架。

这些选择属于 RISC-V 架构层，可在 QEMU `virt` 和 VisionFive 2 上复用。当前 timer 通过两边共有的 SBI TIME 抽象复用，实际 timebase 仍从 DTB 获取；外部中断控制器和设备 IRQ 编号属于平台层，不能从 QEMU 的行为推断开发板布局。时间机制的完整解释见 [RISC-V 时间与周期 Tick 学习总结](riscv-time.md)。

## 怎样验证入口确实可恢复？

只看到 handler 打印日志不能证明返回正确。BoarOS 的验证组合覆盖：

- 在汇编中给寄存器设置独立的完整 64 位哨兵，trap 返回后先把完整现场快照到栈，再逐项比较 x1..x31；SP 和 GP 与进入探针前的实际值比较。高 32 位也必须变化，才能发现错误的 32 位 load/store 导致的截断或符号扩展。
- 用显式 32 位 `EBREAK` 验证同步异常修改 `sepc` 后不会重复执行。
- 用真实 SSIP 验证 interrupt cause、SPP/SPIE/SIE、pending 清除以及 `sret` 恢复 SIE。
- 用真实 SBI timer 验证生产 cause 5 handler 重设 deadline、保持 `sepc` 并至少两次返回高半区 `wfi`。
- 在两个独立启动测试中分别构造 SPP 无效和保存 SIE 开启的 Frame，证明任一返回检查缺失都会失败，而不是让另一项错误掩盖分支覆盖不足。
- 检查最终测试 ELF 的入口反汇编，确认 Frame 槽地址准备、dummy `sc.d`、`sepc` 写回和 `sret` 的顺序；这验证的是实际链接产物，不依赖源码中是否还保留同名文本。
- 在 Bare 低地址和最终 Sv39 高半区各执行真实返回；高半区测试随后再触发未处理 breakpoint，证明 fatal fallback 仍在。
- 保留 store/load page fault 测试，防止 Frame 重构破坏原始 `scause`、`sepc`、`stval`、`sstatus` 诊断。
- 让真实 U-mode 任务在独立 Sv39 根页表中运行，由 timer 抢占到内核线程后再恢复，逐项核对用户 `gp/sp/tp/s0..s11`，同时证明内核执行期间 `sscratch=0`；最终 ELF 反汇编还要证明入口确实重载了内核 `gp`。
- 让第二个用户地址空间触发 load page fault，验证故障只形成任务完成记录，而不会破坏另一用户任务或把 S-mode 故障错误降级。
- 让真实 ext4 `/init` 在初始栈提交区以下先 load 零页、再 store 另一页，证明 cause 13/15 能补页并保持 `sepc` 重试；测试内核再注入一次 OOM，要求父进程得到 wait status 9 且所有资源回到基线。
- 让真实静态 musl 使用 F/D 寄存器跨 timer 抢占和 signal handler/sigreturn，检查独立 FP image 没有泄漏；同时让 pipe 和 nanosleep 在 pending signal 下唤醒，验证公共 return tail 的 EINTR/SA_RESTART 路径。

## 资料依据

- `references/riscv/riscv-privileged-20260120.pdf`：S-mode CSR、trap 进入、`SRET`、中断条件和 `sscratch`。
- `references/linux/arch/riscv/kernel/entry.S`：完整 `pt_regs`、`sscratch`/`tp` 切换、dispatcher、返回和 dummy `SC`。
- `references/linux/arch/riscv/include/asm/ptrace.h`：Linux RISC-V 整数 trap 上下文布局与用户/内核来源判断。
