# RISC-V 浮点与向量状态模块

本文记录用户态 F/D 与 RVV 1.0 扩展状态在 BoarOS 中的所有权、调度切换和信号/exec 边界。整数 Trap Frame 的入口与返回契约见[RISC-V Trap 模块](riscv-trap.md)。

## 范围与状态布局

| 文件 | 当前职责 |
|---|---|
| `include/arch/riscv/fpu.h` | per-task FPU state 的布局、偏移和汇编接口 |
| `arch/riscv/fpu.S` | 唯一允许触碰 F/D 寄存器的保存、恢复、切换和 reset 代码 |
| `kernel/sched/core.c`、`kernel/sched/process.c` | 创建、调度、clone 和退出时的 FP owner 生命周期 |
| `kernel/sched/exec.c`、`arch/riscv/signal.c` | exec 清空状态、signal frame 保存/恢复 |
| `tests/userland/real.c` | 真实 U-mode FP 抢占和寄存器保持测试 |

每个用户 task 的 `struct riscv_fpu_state` 包含 32 个 64 位寄存器、`fcsr` 和一个 `saved` 标志，共 272 字节、16 字节对齐。基础整数 Trap Frame 仍为 288 字节，不把 FP 状态混入每次 trap 保存；FP 状态由当前 task 单独拥有。

## FS 状态机与调度

初次 ELF 入口和 exec 的 Frame 将 `sstatus.FS` 置为 Initial；fork 继承父进程实际 FP image，首次调度加载该 image。只有汇编封装 `riscv_fpu_state_save/restore` 和 `riscv_fpu_switch` 使用 F/D 指令；内核 C 和普通汇编按 RV64IMAC 编译，不在 FP owner 之外触碰浮点单元。

context switch 在关闭 SIE 的临界区先观察 FS：前一个用户 task 为 Dirty 时保存 32 个寄存器和 fcsr、将 FS 收敛到 Clean，再按需加载下一个 task 的 saved image；恢复完成后保持 Clean，使下一次用户 FP 指令重新产生 Dirty。内核线程没有用户 FP owner，不保存也不恢复。task 退出时丢弃其 FP image，只加载下一个用户 task 的状态。

返回汇编调用的最后准备点从live CSR合并FS到当前Trap Frame，其余sstatus位仍由Frame决定。trap内调度、信号恢复和exec可能改变live FS，不能把入口旧快照写回，也不能无条件标成Clean：没有保存过的Dirty状态仍须让下一次切换保存。用户返回与内核抢占返回共用这一步。

这条lazy路径把272字节保存和32次store/load放在真正使用FP的task切换上，而不是每次整数trap都支付。两核配置由调度域与on_cpu交接保护task image，目标核在旧执行owner结束后才加载；没有跨hart延迟借用仍在另一核运行的FPU owner。

## exec、clone 与 signal 边界

fork/clone 先保存父进程当前硬件 FP 寄存器，再复制 32 个 D 寄存器、fcsr 和 saved 状态到 child。不能仅复制可能过时的内存 image，更不能把 fork 当成 exec 清零。exec 在提交新 MM 和 Trap Frame 前调用 reset：硬件 32 个寄存器、fcsr 和内存 image 都清零，并让新程序从 FS Initial 开始，避免旧映像的 callee-saved FP 值泄漏。

信号 frame 的 ucontext 保存当前 task 的 32 个 D 寄存器和 fcsr；`rt_sigreturn` 先通过用户 frame 校验，再把它恢复为下一次调度可加载的 per-task image。FP 仍不改变整数寄存器的 ptrace 顺序和 Trap Frame ABI。

## 当前边界与验证

当前 F/D 使用 64 位寄存器视图；RVV 1.0 使用下述独立、按需分配的状态。未实现用户态浮点异常信号、跨 hart lazy ownership、Zve 子集单独启用或旧版 XTheadVector。F/D 每个 task 固定承担 272 字节；实际吞吐、板级性能及 trap 成本未测量，QEMU 正确性通过不表示性能收益。

```sh
make test-userland-riscv
make test-smp-fpu-setup-riscv
make test-riscv
```

真实用户程序让浮点寄存器装入不同模式，在timer抢占、handler/sigreturn、clone/exec和退出回收后检查值；signal测试覆盖ucontext FP区域与恢复边界。SMP fixture在四个真实owner上检查返回准备保留live Off/Initial/Clean/Dirty，同时覆盖timer抢占、迁移和最终image归还。

## RVV 1.0 的硬件和任务边界

`kernel/dtb.c` 对可运行 hart 的标准 `v` 扩展取交集；新的
`riscv,isa-extensions` 属性优先于旧 ISA 字符串。没有 V、任一启用 hart
缺少 V、或只有 Zve/XTheadVector 时不访问向量 CSR、不宣告 HWCAP V，用户指令
按普通非法指令交付 SIGILL。启动 hart 读取 VLENB，接受 16..8192 字节的 2 次幂。
每个 secondary 在上线前核对相同 VLENB；异长 hart 当前拒绝启动，不允许静默迁移
并截断状态。此核对发生在用户调度之前。

`arch/riscv/vector.c`、`vector_state.S` 管理独立于 F/D 的 owner。每个 task
只固定增加状态指针和控制字，首次 V 指令（包括向量 CSR 访问）才分配
`48 + 32 * VLENB` 字节。分配失败不置 VS，不执行指令，而交付 SIGBUS（SI_KERNEL）。
普通整数任务之间切换不访问向量 CSR，也不分配或搬移寄存器。

调度只保存 Dirty 的前任；后继有状态时恢复，否则关闭 VS。32 个寄存器用
whole-register 指令搬移，不依赖用户 VL/VTYPE、尾部策略或 mask；搬移期间
清零 VSTART，随后恢复 VSTART/VL/VTYPE/VCSR。返回准备合并 live VS，不能把旧
trap 快照覆盖已经保存的状态。普通切换、退出的 discard switch、CPU idle
转换都遵守同一规则；没有跨 hart 借用仍在运行的 owner。

clone 复制已同步的父 image，分配失败让 clone 回滚。exec 关闭 VS、归还旧 image，
首次使用新映像时清零全部寄存器及 CSR（含 VTYPE）；最终 task 回收释放状态，包括未发布 clone
的错误路径。`PR_RISCV_V_SET_CONTROL/GET_CONTROL` 支持当前/下次 exec 的
ON/OFF/default 和 inherit；已经 ON 的当前映像不能转为 OFF。默认允许 V。
`AT_HWCAP` 只在上述硬件前提满足时宣告 V；`AT_MINSIGSTKSZ` 包含最大向量信号帧。

## RVV signal 与 ptrace ABI

ABI 依据仓库固定 Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的
`arch/riscv/include/uapi/asm/{ptrace,sigcontext}.h` 和 `kernel/{signal,ptrace,vector}.c`。
没有向量状态时保留原 1088 字节 signal frame。有状态时，FP union 尾部 header
为 magic `0x53465457`、大小 `56 + 32 * VLENB`；后面是 6 个 64 位字段和
由用户地址 `datap` 指向的寄存器字节，最后有全零 END header。帧按 16 字节对齐。

sigreturn 校验 reserved/header/size、完整扩展链和用户可读范围，允许 datap
独立迁移和重复已知记录（最后一份生效）。VLENB 只由硬件决定，不作为用户提供的
拷贝长度。先将新 image 完整读入临时 bank，再交换 owner 并恢复，避免用户读取
失败或期间调度发布半份状态；不额外清零或再复制整份 bank。坏帧交付 SIGSEGV。

`NT_RISCV_VECTOR` (`0x900`) 的 ptrace regset 使用 5 个 64 位 CSR 字段加
**内联**寄存器数据，不含 datap；没有首次使用的 task 返回 ENODATA。
GET 从停止且被独占借用的 image 直接复制，不分配第二份向量 bank。
SET 支持部分覆盖，在完整快照上校验 VLENB、VTYPE、VCSR 和 VL 后提交，
不受通用 ptrace 单页 scratch 大小限制。core 文件目前仍沿用已有的 GPR notes，
没有新增向量 core note；`riscv_hwprobe` 和 V 默认策略 sysctl 不在本次实现范围。

## 定向验证入口与限制

```sh
make test-riscv-vector-host
# 使用已有 RV musl 工具链；普通 C 保持 RV64GC，只有 .S 开启 V。
$RVCC -O2 -static -march=rv64gc -mabi=lp64d \
  tests/userland/riscv-vector.c tests/userland/riscv-vector.S -o build/riscv-vector-user
python3 tests/userland/riscv-vector.py --kernel kernel-rv \
  --program build/riscv-vector-user --vlen 128
# --vlen 0 验证无 V；另测 256/1024。SMP 内核加 --cpus 2。
```

host 在 4K/16K 页模型中运行生产 C 路径，覆盖所有合法 VLENB、异 hart 长度、
首次/clone/ptrace 分配失败、资源归还及 CSR 溢出输入。实际 QEMU guest 覆盖
首次清零、完整寄存器与 CSR、alternate stack 上的嵌套信号、外部 datap、坏帧、ptrace、fork/exec、
prctl，以及双 hart 绑核后通过共享原子握手确认的不同 owner 隔离。
专用 `RISCV_VECTOR_OOM_FIXTURE=1` link-wrapper 内核与 `-DVECTOR_OOM_FIXTURE`
用户程序还在真实首次使用和 clone bank 分配点各注入一次失败：检查 SIGBUS 的
SI_KERNEL、返回后成功重试、fork 的 ENOMEM、之后的成功 fork 以及最终资源归还。
runner 加 `--clone-oom` 才验收这些注入标记；正常内核不含 wrapper。
runner 保存命令和 kernel/程序 SHA-256，验证结束时输入未改变及 heap-live 归零。

QEMU 10.0.13 仅支持 VLEN 128..1024，最大 256KiB bank 和异长 hart 的处理
仅由 host 模型覆盖，不声称有对应硬件验证。未找到可复用 Linux RV V guest
内核，故没有 Linux 动态差分；上游源码核对不替代运行对照，也不代表原生硬件验收。
