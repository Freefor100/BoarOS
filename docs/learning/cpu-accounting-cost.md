# CPU 记账对程序耗时的影响

## 结论

记账位于共同trap路径，影响系统调用、中断和任务切换。影响程序总时间的程度取决于这些操作的频率。getpid自身工作很少，固定成本占比高；计算和SQL执行的大部分时间仍在用户态，事务还包含实际日志与同步。

当前实现由CPU持有未结束的硬件counter片段，在查询或切栈时换算并合并到任务纳秒统计。常规trap更新使用已有的本地IRQ关闭范围；跨CPU查询固定owner后读取一致快照。原先每次进出均取全局调度锁并换算纳秒的路径已替换，单指令counter访问也直接内联。机制集中见[任务资源记账](../modules/kernel-accounting.md)。

这保留了精确记账，普通进出仍各需要一次counter采样和模式更新。原记账版相对P1-B前的差值、优化带来的变化，均按下面同批三版本比较；表内保留整条系统调用耗时。

## 同批对照方法

- `pre-p1b`：main `6b84d38`，P1-B工作之前；`locked`：`31dd8ed`的完整记账实现；`local`：`2f42294`、`db42593`的CPU本地片段和counter访问优化。
- 同一架构内，三个核使用完全相同的程序ELF和SQLite CLI。单CPU、1GiB，固定宿主CPU14/15，诊断关闭。五轮轮换版本顺序，架构和版本之间均串行计时。每次从独立盘启动，核对内容、退出和正常e2fsck。
- RV实际QEMU11.1.2及显式OpenSBI；LA实际QEMU11.1.0 RTC版。程序、核、QEMU、固件、affinity及完整命令由结果文件记录。不同架构使用各自的ELF和平台，表内按同架构比较。

## 短系统调用

使用最初保存的同一raw syscall ELF，计时1万次getpid及1000次sched_yield。下表为五轮中位数，单位ms。原先报告的44.7%来自此前未固定宿主CPU位置的一批；本表重新在同一批比较三个版本。

| 工作量 | P1-B前 | 原记账版 | 优化版 | 优化版相对原记账版 |
|---|---:|---:|---:|---:|
| 10000次getpid | 27.976 | 42.054 | 34.286 | -18.5% |
| 1000次sched_yield | 5.691 | 6.905 | 5.938 | -14.0% |

getpid每次从约4.205µs降到3.429µs。相对P1-B前，本批额外耗时由50.3%降至22.6%，约去掉一半额外成本。sched_yield相对P1-B前为+4.4%。这些是整条调用的同批差值，保留了新旧源码其他公共路径的差异。

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

原始资料保存在`build/cpu-account-opt/`：`micro-current`、`apps-current-rv`、`apps-current-la`、`apps-confirm-la`是本页计时输入；各目录的results.json、逐轮串口和fsck日志包含完整数值。`final-usage-*`、`final-wait-*`及`final-lifecycle`为最终正确性结果。较早的中间实现测量按原核哈希独立保留，正式结论使用本页列出的批次。检查后的运行盘删除，保留构建缓存、ELF和日志。
