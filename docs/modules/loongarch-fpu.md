# LoongArch 浮点与SIMD状态

范围为QEMU virt/LA464、单CPU、LA64、32个最大256位寄存器、8FCC和FCSR；
内核C保持LP64S整数ABI，用户可运行原版LP64D musl。LBT、SMP和实板不在范围。
固定依据：`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的`arch/loongarch/kernel/{fpu.S,signal.c,traps.c,process.c}`、`include/asm/asmmacro.h`
与UAPI sigcontext；QEMU v11.1.0 commit`84f07211cc5b4fc6a371559bf8a5de4fb068e648`
和卷一v1.11提供CPUCFG/EUEN/异常事实。

## Owner、首用与生命周期

每task内嵌1056字节image，按32字节对齐：32×4个64位lane、FCC、FCSR、width、
saved和live_width。width为当前硬件owner宽度0/1/2/4，live_width为已初始化宽度；
saved对应Linux used_math/SC_USED_FP，不能拿它替代硬件owner判断。sigreturn可
撤销saved而保留SIMD live，因此context切换/clone按width快照硬件。

异常15/16/17分别按FP/LSX/LASX首用或升级恢复；CPU不提供扩展时为用户SIGILL。
首次需要初始化的FR/lane按固定Linux填全1NaN，已live的上半部来自任务image。
使用较窄指令或恢复较窄记录不能无条件丢弃已初始化上半部。宽度合法性和owner
损坏仍fatal，首次使用不分配内存。只在受控汇编触碰向量寄存器；每次切换保存
当前启用宽度，恢复后一owner，整数任务EUEN关闭。无SMP或性能等价声明。

clone先快照父硬件，按固定Linux`copy_thread`撤销子任务SIMD live：已used的
子任务保留标量FR/FCC/FCSR，向量上半部在首次使用重新初始化，不声明完整向量
位模式继承。exec清空所有状态，新映像首用重新初始化，旧FR/CSR不能泄漏。
HWCAP由CPUCFG给出已实现的FP/LSX/LASX；关闭扩展的CPU profile不发布相应位。

## 信号帧与恢复

固定前缀仍576字节；END只读magic/size共8字节。saved未置位时交付592字节整数
帧；否则发布最高live宽度的FPU、LSX或LASX记录。magic依次为0x46505501、
0x53580001、0x41535801。payload有效字节分别268/524/1036，ABI最小记录尺寸
分别288/544/1056，尺寸包含尾padding。交付按Linux从SP向下分配END、对齐
payload和info；LASX payload按32字节，其余至少16字节，帧最大1664字节。

恢复不额外要求用户帧对齐或padding可读。重复的同类记录取最后一条；不同类
优先LASX→LSX→FPU，不受排列顺序影响。先快照GP/mask/选中有效payload再
提交；未知或过短记录为坏帧。保留实际启用宽度和已有live信息，不能仅凭用户
提供的更宽记录自动启用ISA。撤销SC_USED_FP时丢弃handler硬件更新，保留原
内存image及已live上半部；随后向量首用按固定Linux恢复，不简单重置为NaN。
PRMD和内核TP始终来自可信trap。

FPE清除Cause与Enable相交的位，但si_code按原始全部Cause优先级选择：
invalid、divide、overflow、underflow、inexact，si_addr为ERA。仅启用inexact的
实际溢出/下溢仍报告FPE_FLTOVF/FPE_FLTUND。sigreturn选中FCSR存在启用pending
时先写回清除后的用户FCSR，再交付SI_KERNEL SIGFPE；只读/坏输出是坏帧。

## 验证

`make test-fpu-loongarch`继续覆盖原标量fenv/算术、全部FR/FCC/FCSR、timer、
fork、exec、真实FPE、pending及FP/END不可读padding。`make test-simd-loongarch`
在Linux/BoarOS的512MiB/1GiB运行同一ELF，覆盖全部lane、纯计算timer抢占、
信号修改及硬件clobber、宽度初始化、真实clone的标量继承/上半部初始化、exec
旧状态清除、坏/短/重复/混合记录、嵌套、不可读padding、清除SC_USED_FP以及
pending/只读写回失败。另跑LASX关闭及LSX/LASX均关闭profile，核对实际故障与
HWCAP；退出须回到根owner与页/堆基线。

标量/整数信号、线程创建OOM、动态musl/TLS、根盘与两架构栈门禁继续保留。
没有穷举所有IEEE/vector运算，也没有深层调用链最大栈界或性能验收。
