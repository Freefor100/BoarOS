# LoongArch 标量浮点状态

范围为QEMU virt/LA464、单CPU、LA64、32个64位FR、8个FCC和FCSR；内核仍为
LP64S整数ABI，用户可使用原版LP64D musl。LSX/LASX/LBT、SMP和实板不在此范围。
固定依据为 `references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的 `arch/loongarch/kernel/{fpu.S,signal.c,traps.c}`、`include/asm/asmmacro.h` 与
UAPI sigcontext；硬件与异常依据为已固定卷一v1.11和QEMU v11.1.0。

## Owner 与切换

每task内嵌288字节image：FR[32]、按每FCC一字节编码的64位FCC、32位FCSR、
padding及saved标记。saved在LA表示该task已使用FP，当前task的最新值可能
仍在硬件；不能直接把旧内存image复制给clone。整数trap仍为304字节。

初始/exec把saved和EUEN清零。首次用户FP Disabled异常15重试原ERA，创建
独立image、按Linux初始化FR为全1的NaN，FCSR/FCC清零并仅启用EUEN.FPE。
已使用FP的task在已有arch_fpu_switch中保存前一owner、恢复后一owner；
内核线程和未使用FP的用户task保持EUEN关闭。LA没有RV的FS Dirty位，首次
使用后每次切换保存，未声称与RV具有相同成本。只在fpu_state.S及受控FCSR
封装触碰硬件；内核C不用浮点运算。当前单CPU、中断关闭，不是SMP owner协议。

clone先快照父硬件再复制image；exit丢弃退出者，只恢复下一owner。exec重置
旧状态并关闭硬件，新映像第一次使用重新初始化，不把旧FR/FCSR泄漏给新程序。

## 信号与错误

未使用FP的交付帧仍为592字节END整数帧；使用后为880字节，SC_USED_FP置位，
FPU记录magic=0x46505501、size=288（16字节header、268字节有效字段及4字节
padding），后接END。有效字段为32FR、64位FCC和32位FCSR；恢复只快照这268字节，
不要求尾部padding可读，最小记录尺寸仍为288，也接受更大尺寸跳到后续END。
恢复先快照全部GP/mask/扩展输入，随后提交；END仅读取magic/size，不额外要求
用户帧对齐或END padding可读。未知扩展和太短记录为用户坏帧，不能fatal内核。
没有发布SIMD能力。

FPE陷入时按Linux清除Cause与Enable相交的位，但从原始全部Cause中选择SIGFPE
的si_code：invalid优先，其后divide、overflow、underflow、inexact，si_addr为ERA。
因此只启用inexact的溢出/下溢运算仍报告FPE_FLTOVF/FPE_FLTUND。sigreturn中的已启用pending
Cause先写回清除后的用户FCSR，再交付SI_KERNEL来源SIGFPE；只读/坏输出是坏帧。
PRMD和内核TP不来自用户。用户FP错误由原共用信号策略处理，allocator/page
owner损坏仍fatal。

## 验证

`make test-fpu-loongarch` 在Linux和BoarOS的512MiB/1GiB运行同一原版musl
LP64D静态ELF，验证算术/fenv、全部FR/FCC/FCSR、两个计算线程的timer保持、
fork继承、handler修改硬件与用户image、exec初始化、真实除零及仅启用inexact的
溢出/下溢SIGFPE、未知/短扩展帧、sigreturn pending，以及END/FPU尾部padding
跨到不可读页。根owner和页/堆必须
恢复基线。原LP64S信号/线程、LA启动/根盘和两架构栈门禁继续运行。

原版动态musl/TLS的组合验证见[LA模块](loongarch-boot.md)。没有FPU性能测量，
没有声明完整IEEE异常组合或深层栈调用链上界。
