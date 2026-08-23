# RISC-V Timer 与内核 Tick 模块

本文描述当前 RISC-V 定时器、生产 timer trap 和架构无关 tick 计数的稳定接口。硬件机制、性能含义和项目选择见 [RISC-V 时间与周期 Tick 学习总结](../learning/riscv-time.md)，Trap Frame 与返回契约见 [RISC-V Trap 模块](riscv-trap.md)。

## 范围与入口

| 文件 | 当前职责 |
|---|---|
| `kernel/dtb.c`、`include/kernel/dtb.h` | 从根节点直属 `/cpus` 读取原始 `timebase-frequency` |
| `arch/riscv/sbi.c`、`include/arch/riscv/sbi.h` | 探测 SBI 扩展并通过 TIME 设置绝对 deadline |
| `arch/riscv/timer.c`、`include/arch/riscv/timer.h` | 维护单 hart timer 状态、读取 `time`、计算和重设 deadline、开启 STIE/SIE |
| `arch/riscv/trap.c` | 处理 supervisor timer interrupt 并把 elapsed tick 交给通用层 |
| `kernel/tick.c`、`include/kernel/tick.h` | 维护 100 Hz 策略使用的 64 位累计 tick |
| `kernel/main.c` | 在最终 Sv39/direct map 建立后启动 timer，并进入 `wfi` idle |
| `tests/riscv/timer_cases.c`、`tests/timer-riscv.sh` | 验证状态、deadline 数学、SBI 错误与真实 QEMU timer trap |
| `tests/riscv/timer_boot.c` | 只在测试 ELF 中累计真实中断并于有限 tick 后关机 |
| `tests/idle-riscv.sh` | 验证正常内核启动 timer 后持续 idle |

通用 tick 接口为：

```c
#define KERNEL_TICKS_PER_SECOND 100U

void kernel_tick_advance(uint64_t elapsed_ticks);
uint64_t kernel_tick_count(void);
```

当前计数使用无符号 64 位模运算。它没有测试重置、回调、调度器入口、锁或 per-hart 存储；调用者可以一次报告多个迟到周期。

RISC-V timer 接口为：

```c
enum riscv_timer_status riscv_timer_start(
    uint32_t timebase_frequency,
    uint32_t ticks_per_second);

enum riscv_timer_status riscv_timer_handle_interrupt(
    uint64_t *elapsed_ticks);
```

成功启动只允许一次。失败状态区分参数或频率错误、重复启动、SBI probe 失败、TIME 不可用、set_timer 失败、未启动和提前中断。输出参数只在整个处理成功后写入。

## 启动和中断不变量

启动汇编先保持 `sie=0` 和 `sstatus.SIE=0`。`kernel_main` 完成最终页表激活、direct-map 访问绑定和启动期运行检查后，才把 DTB 频率与 100 Hz 策略传给 timer：

1. 频率和 tick 频率必须非零，且 timebase 不低于 tick 频率；
2. 通过 SBI Base probe 确认 TIME 扩展可用；
3. `period = timebase_frequency / ticks_per_second`；不能整除时使用整数商；
4. 读取 `time`，先通过 SBI 设置第一个未来绝对 deadline；
5. SBI 成功后才提交 `{period, next_deadline, armed}`；
6. 最后依次开启 `sie.STIE` 和全局 `sstatus.SIE`。

因此初始化失败不会留下已 armed 的软件状态，也不会由本模块提前打开中断。QEMU `virt` 的 10 MHz 和 VisionFive 2 的 4 MHz 均能被 100 整除，对应周期分别为 100000 和 40000 个 timebase 计数。

Direct-mode dispatcher 精确匹配：

```text
scause = interrupt bit | 5
```

timer trap 进入时硬件已把旧 SIE 保存到 SPIE 并清 SIE，当前 handler 不重新开启嵌套中断。它不修改 `sepc`；在 SBI 安排新的未来 deadline 后返回，Trap Frame 恢复路径最终通过 `sret` 回到被中断位置和 `wfi` 主循环。所有其他未知 cause 仍进入原有 fatal 诊断。

## Deadline 与失败原子性

handler 不使用 `now + period`，而是保持上一计划相位：

```text
delta   = now - previous_deadline
elapsed = delta / period + 1
next    = previous_deadline + elapsed * period
```

若 `now` 恰好等于旧 deadline，elapsed 为 1；若已经跨过多个周期，一次报告全部 elapsed，复杂度仍为 O(1)。这样 handler 耗时不会逐次累加成时钟漂移，也不会循环补发大量中断。

时间先后按 64 位模运算判断，要求两次有效处理的距离小于 `2^63` 个 timebase 计数。成功计算保证新 deadline 位于采样的 `now` 之后。候选 deadline 和 elapsed 先保存在局部变量；只有 SBI set_timer 成功后才提交新状态和输出。若固件调用失败，dispatcher 输出 timer 状态并关机，不以旧状态伪造成功。

## 验证与限制

```sh
make test-dtb-riscv
make test-timer-riscv
make test-idle-riscv
make test-trap-return-riscv
make test-riscv
```

合成 DTB 覆盖缺失、错误位置、错误长度、零值、重复属性和失败输出不变。timer cases 覆盖全部启动状态，以及恰好到期、周期内迟到、跨周期迟到、无效输出和 `UINT64_MAX` 回绕。真实 QEMU 测试使用实际 DTB、`time` CSR、OpenSBI TIME 和 Trap Frame 返回路径，至少两次返回 `wfi` 后由测试链接包装在第三个累计 tick 关机；正常 `kernel-rv` 不含有限退出逻辑。

当前限制为单 hart、固定 100 Hz 周期策略和 SBI TIME 后端。尚无 scheduler、线程唤醒、超时队列、tickless、Sstc 直写、外部中断、IPI、per-hart timer 或 LoongArch timer。正常 tick 不写 UART；日志只出现在初始化和错误路径。
