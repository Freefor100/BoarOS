# CPU 记账对程序耗时的影响

## 结论

记账位于共同trap路径，影响系统调用、中断和任务切换。影响程序总时间的程度取决于这些操作的频率。getpid自身工作很少，固定成本占比高；计算和SQL执行的大部分时间仍在用户态，事务还包含实际日志与同步。

当前实现由CPU持有未结束的硬件counter片段。查询时将它换算后与任务累计相加；切栈时才将片段写入任务纳秒统计。常规trap更新使用已有的本地IRQ关闭范围；跨CPU查询固定owner后读取一致快照。原先每次进出均取全局调度锁并换算纳秒的路径已替换，普通单指令counter访问直接内联。机制集中见[任务资源记账](../modules/kernel-accounting.md)。

这保留了精确记账，普通进出仍各需要一次counter采样和模式更新。原记账版相对P1-B前的差值、优化带来的变化，均按下面同批三版本比较；表内保留整条系统调用耗时。

## 同批对照方法

- `pre-p1b`：main `6b84d38`，P1-B工作之前；`locked`：`31dd8ed`的完整记账实现；`local`：`2f42294`、`db42593`的CPU本地片段和counter访问优化。
- 同一架构内，三个内核版本使用完全相同的程序ELF和SQLite CLI。单CPU、1GiB，固定宿主CPU14/15，诊断关闭。宿主为i5-12500H，CPU0–7为P核逻辑CPU，CPU8–15为E核；本节五轮数据均在E核上取得。五轮轮换版本顺序，架构和版本之间均串行计时。每次从独立盘启动，核对内容、退出和正常e2fsck。
- RV实际QEMU11.1.2及显式OpenSBI；LA实际QEMU11.1.0 RTC版。程序、核、QEMU、固件、affinity及完整命令由结果文件记录。不同架构使用各自的ELF和平台，表内按同架构比较。

## 短系统调用

使用最初保存的同一raw syscall ELF，计时1万次getpid及1000次sched_yield。下表为五轮中位数，单位ms。首次报告的15.3896→22.2727ms（+44.7%）来自未固定宿主CPU的一批；后续选择CPU14/15，将运行范围固定到了E核。两个旧核、程序和QEMU的SHA256均与首次一致，启动日志的固件版本及10MHz时钟频率也一致。这里保留E核上的三版本同批比较，并在下节验证绑核造成的差距。

| 工作量 | P1-B前 | 原记账版 | 优化版 | 优化版相对原记账版 |
|---|---:|---:|---:|---:|
| 10000次getpid | 27.976 | 42.054 | 34.286 | -18.5% |
| 1000次sched_yield | 5.691 | 6.905 | 5.938 | -14.0% |

getpid每次从约4.205µs降到3.429µs。相对P1-B前，本批额外耗时由50.3%降至22.6%，约去掉一半额外成本。sched_yield相对P1-B前为+4.4%。这些是整条调用的同批差值，保留了新旧源码其他公共路径的差异。

### 两批绝对耗时为什么不同

只改变QEMU的宿主CPU绑定，使用上表相同的三个内核版本、原程序、显式固件及启动参数，每组串行执行三轮并轮换顺序。核对宿主`cpu_core/cpus`与`cpu_atom/cpus`后，将CPU0/1作为P核组，CPU14/15作为E核组。下表为1万次getpid的中位数，单位ms。

| 宿主绑定 | P1-B前 | 原记账版 | 优化版 |
|---|---:|---:|---:|
| P核，CPU0/1 | 15.440 | 21.160 | 17.726 |
| E核，CPU14/15 | 27.838 | 41.273 | 33.366 |

相同旧核从P核移到E核，耗时从15.44ms变为27.84ms，复现了两批绝对值的主要差距。首次未记录逐次运行的宿主核编号，其耗时与本次P核结果接近。后续选择E核改变了测量条件，原先报告应同时说明这一点。

优化收益也按CPU类型分别比较：本次P核为16.2%，E核为19.2%；优化版相对P1-B前分别多14.8%、19.9%。前一张五轮对照表中的18.5%收益和22.6%剩余差值，适用于原E核五轮批次。所有组均检查完整输出、正常退出及e2fsck，运行盘检查后删除。

## 程序与实际I/O

`tests/accounting/cost.c`固定执行：2亿次整数运算并检查校验值；对640000字节tmpfs文件各做1万次64字节写/读并检查内容；原版SQLite CLI在内存中求50000项之和；SQLite创建5000行，执行40次DELETE journal、synchronous=FULL事务，最后检查integrity_check、行数及总和。SQLite计时包含子进程的fork/exec/运行/回收，完整工作量和同步条件相同。

首批各五轮的中位数如下，单位ms。

| 架构 / 工作量 | P1-B前 | 原记账版 | 优化版 | 优化版相对原记账版 |
|---|---:|---:|---:|---:|
| RV / 整数计算 | 367.699 | 379.163 | 368.529 | -2.80% |
| RV / 64字节小块读写 | 480.166 | 492.841 | 480.176 | -2.57% |
| RV / SQLite内存SQL | 422.780 | 418.416 | 413.917 | -1.08% |
| RV / SQLite同步事务 | 1283.020 | 1296.631 | 1273.032 | -1.82% |
| LA / 整数计算 | 367.310 | 367.431 | 367.793 | +0.10% |
| LA / 64字节小块读写 | 362.906 | 380.460 | 368.042 | -3.26% |
| LA / SQLite内存SQL | 288.296 | 296.162 | 290.667 | -1.86% |
| LA / SQLite同步事务 | 1196.761 | 1172.488 | 1222.152 | +4.24% |

计算负载相对P1-B前约+0.2%/+0.1%，小I/O相对原记账版改善约2.6%/3.3%。SQLite各版本升降较小、波动较大；例如RV计算单次范围367–472ms，RV SQLite内存SQL为411–558ms。程序整体并未呈现getpid微基准所显示的四成级耗时增加。

### LA同步事务的耗时上升

首批该项为+4.24%，因此追加相同ELF、负载和同步条件的五轮原记账版/优化版对照，得到+1.54%。合计十轮，中位数1174.277→1194.453ms，为 **+1.72%**；单次范围分别1138.0–1220.9ms、1133.1–1240.8ms（精确值见原记录）。这个上升保留为本轮的实际结果，事务负载的收益尚不明确；小I/O的改善则在补测中再次出现，为-3.75%。

## 正确性与复查

宿主测试先复现CPU0持调度域时CPU1无法完成自身记账；优化后，CPU1在该交错中完成2万次模式转换，读者逐次核对user+kernel与采样时间一致。另覆盖嵌套IRQ排除、精确栈归属和handoff。RV/LA两种内存配置的54项资源/时钟及58项waitid与固定Linux匹配；RV实际1/2CPU均通过，12次持有查询目标期间的回收/非主线程exec组合启动正常，资源归还及磁盘检查通过。

RV SMP的TIME CSR属于I输入，采样与普通内存序列之间需要显式次序；这是[固定20260120版Zicsr的规定](https://docs.riscv.org/reference/isa/v20260120/unpriv/zicsr.html#_csr_access_ordering)。当前counter接口在采样前约束RW→I、之后约束I→R，实际SMP指令检查覆盖该接口及查询/trap调用者。QEMU通过与ISA顺序检查分别保留。

```sh
make test-sched-concurrency-host test-scheduler-handoff-host test-user-smp-host
make build-accounting-cost
python3 -B tests/accounting/counter-order.py --kernel build/riscv-user-smp/kernel-rv
# 保留旧核后，串行复测固定程序；输出目录选择新目录。
taskset -c 14,15 python3 -B tests/accounting/cost.py \
  --arch riscv --kernel locked=build/cpu-account-opt/before-rv --kernel local=kernel-rv \
  --program build/riscv/tests/user/cpu-account-cost-rv \
  --sqlite build/riscv/tests/user/sqlite3-static-rv \
  --firmware /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
  --output build/account-cost-repeat
```

LA使用`--arch loongarch`、`build/loongarch/cpu-account-cost`及`build/loongarch/sqlite3-static`，对应核为`before-la`和`kernel-la`，按其平台直接启动。

原始资料保存在`build/cpu-account-opt/`：`micro-current`、`apps-current-rv`、`apps-current-la`、`apps-confirm-la`是五轮计时输入；`host-core-check`保留P/E核三轮对照、CPU类型和相同二进制标识；各目录的results.json、逐轮串口和fsck日志包含完整数值。首次15.39→22.27ms的记录位于`build/cpu-usage/matched-benchmark/`。`final-usage-*`、`final-wait-*`及`final-lifecycle`为最终正确性结果。较早的中间实现测量按原核哈希独立保留，正式结论使用本页列出的批次。检查后的运行盘删除，保留构建缓存、ELF和日志。

## 未启用 CPU 间隔定时器的成本

本节比较轮前main `211b101`与补齐VIRTUAL/PROF后的最终版本，全部程序均未开启间隔定时器。固定宿主P核逻辑CPU0/1、客机1CPU/1GiB、诊断关闭；两架构及全部启动串行，程序、SQLite、工作量与最终同步条件相同。每架构五轮轮换`before/after/control`顺序；`control`与`before`是字节完全相同的旧内核，用于观察同核重复运行的差值。表中均为中位数，耗时单位ms，自对照列为control相对before。

新增CPU定时器沿既有timer入口检查启用索引，进出内核的CPU记账没有增加操作。状态置于task尾部，既有热字段偏移保持原值；默认RV task从2864增至2912字节，实际仍分配一张4KiB元数据页。空索引提前返回，省去无用途的时间读取。普通信号返回另拆出完整交付函数：RV无事件入口栈帧由224降至32字节，事件消费、忽略/停止及临时mask的检查顺序保持原语义。

| 架构／工作量 | 轮前 | 最终版本 | 变化 | 同核自对照变化 |
|---|---:|---:|---:|---:|
| RV／1万次getpid | 17.725 | 17.656 | -0.39% | -0.62% |
| RV／1000次yield | 2.735 | 2.778 | +1.61% | -0.64% |
| LA／1万次getpid | 10.826 | 10.900 | +0.68% | +1.52% |
| LA／1000次yield | 1.897 | 1.872 | -1.29% | +0.66% |

| 架构／工作量 | 轮前 | 最终版本 | 变化 | 同核自对照变化 |
|---|---:|---:|---:|---:|
| RV／整数计算 | 186.188 | 185.938 | -0.13% | -0.43% |
| RV／64字节小块读写 | 223.599 | 227.084 | +1.56% | +0.08% |
| RV／SQLite内存SQL | 247.788 | 248.479 | +0.28% | -0.19% |
| RV／SQLite同步事务 | 1069.728 | 1073.422 | +0.35% | +0.10% |
| LA／整数计算 | 183.411 | 182.599 | -0.44% | +0.89% |
| LA／64字节小块读写 | 165.075 | 171.483 | +3.88% | +0.30% |
| LA／SQLite内存SQL | 154.552 | 156.393 | +1.19% | -0.56% |
| LA／SQLite同步事务 | 1037.119 | 1009.474 | -2.67% | -0.30% |

getpid在这组自对照幅度内：RV为−0.39%，LA为+0.68%；计算负载基本持平。仍有实际增加：RV yield约+1.6%，小块读写RV约+1.6%、LA约+3.9%，后者大于本组自对照差值。它们保留为当前剩余成本，具体占比归因尚未完成；未据“没有新增每次syscall操作”推断程序整体零开销。SQLite结果和完整同步条件也保留在上表。

定时器的36项、信号等待31项在RV/LA的512MiB/1GiB与固定Linux匹配，RV另完成1/2CPU；其中两线程各不足180ms、合计超过阈值后触发，验证进程用量合计。1µs周期睡眠复现先在初版失败，再由实际执行owner资格修正；正常BoarOS盘均通过检查。机制及命令见[时间模块](../modules/kernel-time.md#进程-cpu-间隔定时器)和[信号模块](../modules/kernel-signal.md)。

最终记录为`build/cpu-timer-signal/disabled-micro-rv`、`disabled-micro-la`、`disabled-apps-rv`、`disabled-apps-la`，包含各轮原始输出、二进制身份、命令及fsck；`performance-commands-final.json`记录可重建命令。中间候选按`disabled-prelayout-*`、`disabled-layout-before-fast-*`分开保存；`*-overlap`有并行诊断干扰，排除在正式表外。
