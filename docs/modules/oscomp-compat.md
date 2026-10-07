# 双架构原评测运行与评分

本模块仅存在于oscomp-compat。main保留通用内核和自身身份；通用修复先落main再
单向合入此分支。本分支保留兼容uname4.15.0、原盘启动配置、监督与判分。分支
由oscomp-rv-compat本地改名，旧远程引用没有迁移。历史RV专项记录仍保留原身份。
输入与逐次失败的证据边界见[评测学习记录](../learning/oscomp-compat-baseline.md)。

## 固定输入与入口

`tests/oscomp/inputs.json`固定pre-20250615原RV/LA盘和压缩包SHA-256，以及
`references/oscomp-autotest` commit `d1bb3a3c4b27274e196a2648518525c1a304e339`。
原盘只读保留，运行使用独立稀疏副本；`--verify-inputs`显式复查全部四个资产。
新机器显式make prepare-oscomp-inputs恢复固定Harness及SHA绑定资产；普通运行缺失
或身份错误直接失败，不暗中下载替代输入。gzip输入缓存位于build/tools/oscomp，
命中核对解压内容、权限和链接，既有pruner保留该可复用工具输入。
两套镜像自己的musl/glibc和原程序原样执行，不以本机独立验收runtime替换它们。

官方容器参考为`zhouzhouyi/os-contest@sha256:85dec949df7cef41fd03d30c6ad69f952204540e18d2c62bced9d2e262fef12d`，
2026-10-07拉取并核对实际QEMU10.0.2；固定Dockerfile中的9.2.1不是这个镜像的
实际版本。容器LA GNU编译器为13.2.0，宿主当前为15.1.0，各自产物身份独立。
本地派生QEMU和官方容器的结果分别记录，不能只因原judge相同就合称同一环境。
本次实际本地提交与容器运行身份、完整结果摘要及重建入口见
[双架构评测学习记录](../learning/oscomp-compat-baseline.md#本次正式容器结果)。

默认`make all`同时生成kernel-rv/kernel-la。RV/LA分别生成整数raw supervisor、
init.json和头文件；`INIT_CONFIG_RV`/`INIT_CONFIG_LA`独立覆盖，显式共同
`INIT_CONFIG=config/init.json`同时恢复通用fixture。构建中不可让另一组不同配置
覆盖正在冻结的kernel；先构建、复制内核及身份，再运行。

## 原盘bootstrap与监督

PID1直接exec原盘`/musl/busybox`，bootstrap作为sh -c参数编入内核，不由宿主
向原盘注入helper。启动创建/bin、设备、proc和/dev/shm，并从目标profile建立
原loader链接。LA使用/lib、/lib64、/usr/lib64三个实际interpreter路径；RV的
普通/sf musl名称保留。两个libc的LD_LIBRARY_PATH和工作目录分别设置。

默认组顺序为basic、busybox、cyclictest、iozone、iperf、libcbench、libctest、
lmbench、lua、netperf、ltp，各组先glibc再musl。basic/run-all.sh的原发布权限
为0644，bootstrap只补执行位；lmbench的写死辅助路径指向原盘当前libc二进制。
LTPROOT和其testcases/bin在同名Lua test.sh之前，保留原比赛目录遍历与参数。

libc-test原run-static.sh/run-dynamic.sh没有shebang，原LA musl BusyBox在固定
Linux也拒绝直接执行。libctest-hook.sh只在临时外层脚本副本中将这两行改为
原libc自己的BusyBox sh显式执行；原内层脚本、程序、参数、标记和judge不改。
每行必须恰好匹配一次，形状变化返回125而不替换遍历流程。该适配适用于两架构
的两种libc，本地及新官方report以script_adaptations记录是否选中及hook内容哈希；
回收旧冻结运行时仍保留旧身份，不能把新适配归给1915历史成绩。

LTP只在临时副本的唯一执行行接入现有监督，不替换上游runtest或输出测例答案。
监督持有直接child和仍存活的进程组身份；握手放行前退出测试组，child可正常
setsid且kill(0,signal)不会终止外层包装器。native退出/信号状态原样传递；默认
300秒后TERM，2秒后必要时KILL/reap，超时返回124，setup错误125并报告阶段。
直接child回收后不再用其PID，只清理仍属隔离组的后代；脱离组的daemon最终归
PID1/root退出清理，不声明单项监督已经回收它。

源码依据的无参数控制器helper由ltp-skips.tsv列出，返回125并输出SKIP原因，
不生成Summary或通过分；有限压力程序不能按耗时归入该表。显式人工排除另标
EXCLUDE。原LA BusyBox1.33.1的hush不支持set -u/-f；自有脚本按需要显式检查失败，并用不展开
路径的literal空格拆分；hush的while正常结束会触发-e，不能借此跳过后续exec。参数检查和超时/跳过契约保持，原BusyBox和测例不修改。

该结果称为“带监督适配的兼容基线”。原judge的Accepted标签、脚本退出0或
组END不能证明全部子项通过；旧LTP的TFAIL仍可能伴随退出0，helper也可能无
有效断言。控制器、上游缺陷、有限工作量、未到达目标接口和实际内核错误分别核对。

## 诊断与结果

`tests/oscomp/run.py --arch riscv|loongarch|both`默认both。一次调用先冻结全部
内核和fixture，再同时启动；每架构一次启动，两种libc共有22组，双架构44组状态。
本地入口始终是diagnostic，记录实际QEMU内容/模式/路径、启动参数、kernel/helper/
bootstrap/fixture身份、真实串口和退出原因。增加RNG或改变预算须显式标记。
默认仍遵循原Harness设备参数，无RNG时不伪造可信熵ready。

使用原parser、22个judge和postwork。联合summary来自本次两份实际输出，LTP
在联合原始分上调用原非线性公式，不把独立整数分相加。分组状态区分not-selected、
not-reached、incomplete、timeout、script-failure和completed，另保留监督计数。
外层已ENTER但装载包装器失败不能误记未到达。原分数与语义判断、owner证据分开。
自然结束才核对PID1状态和根资源基线；被总预算杀死的启动回收证据为未验证。
输出目录必须新建且位于build内；错误、中断和参考侧失败不能补记通过。

```sh
make prepare-oscomp-inputs  # 显式恢复；普通运行不自动下载
make all
make test-oscomp-host
make test-oscomp-supervisor-riscv test-oscomp-supervisor-loongarch
make diagnose-oscomp-clock-errno # 原RV静态glibc的errno写入归因，独立诊断
python3 -B tests/oscomp/run.py --arch both --groups environment --output build/oscomp-environment-check
python3 -B tests/oscomp/run.py --arch loongarch --groups ltp --output build/oscomp-la-ltp-check
make test-oscomp-riscv       # 本地RV全量诊断
make test-oscomp-loongarch   # 本地LA全量诊断
make test-oscomp-compat      # 同时启动两架构的全量诊断
make test-oscomp-official    # 官方容器入口
make all INIT_CONFIG=config/init.json  # 通用回归fixture
make all                              # 恢复双架构评测默认配置
```

官方配置取自固定Harness的kernel/judge/config.json：单CPU、1GiB、3600秒。
分组诊断、预算覆盖、逐组重启和人工排除不能拼接正式总分。原parser可对未到达
组给出零结果，报告必须保留未到达；不能因44格齐全就声称全部程序执行完毕。

## 验证与证据边界

bootstrap host门禁验证目标loader、helper架构拒绝、独立配置和显式监督参数。
显式shell门禁验证只改变两个入口、保留非零子脚本后续进展及拒绝异常脚本形状。
同一原BusyBox与runtest在固定Linux/BoarOS的512MiB/1GiB验证直接执行ENOEXEC、
显式解释后进入原案例；runtest打印Pass时包装器仍可能退出1，不能把输出当退出0。
真实supervisor使用同一raw ELF及原盘BusyBox对照固定Linux/BoarOS，覆盖native
退出、信号、组隔离、setsid、TERM无效后的KILL/reap、后续进展和源码helper跳过。
Linux对照bootstrap为程序建立真实正进程组，因为直接内核init可继承PGID0。
这些probe仅是诊断fixture，不是官方镜像结果。GNU空LOAD问题在main修正后合入，
不通过修改链接产物绕过exec差异；背景见[ELF记录](../learning/elf-loading.md)。

原测例basic的mount/umount需要vfat和/dev/vda2，LTP还可能需要scratch/loop设备、
账户、工具和真实熵；准备依赖与缺失内核功能分开。原kill判分标签与实际后台PID、
iperf连续脚本在Linux也有的listener重建竞态，以及cyclictest历史零采样，保留各自
归因边界；历史证据见[程序清单](../learning/user-program-inventory.md)与
[预算记录](../learning/data-path-budget-experiments.md)。本阶段不要求清空所有有效
LTP能力缺口，也不修改原程序/libc/judge获取通过。

各阶段记录结论、固定身份和重建命令；运行输出在忽略的build。未解决现场核对前
保留，收口后先预览再make prune-build，原盘、工具、容器和可复用缓存保留。
2026-10-07正式容器已按固定digest完成一次单独运行并收齐原判分及44组报告：
原postwork整数分1915，40组外层正常结束，RV/LA各有一个LTP-glibc预算中断，
两个LTP-musl未到达。两侧无新内核fatal，均观测到QEMU运行超过3600秒；
因此`baseline_established=true`、`all_scripts_completed=false`、
`program_matrix_passed=false`。两侧均由总预算终止，正常PID1及页、堆、任务栈、
根盘和设备owner回收未验证。该原分是本次单独联合成绩，不表示44组全过。
44项分组score的完整精度及总分公式见
[原Job成绩表](../learning/oscomp-dual-official-results.tsv)和
[预算/监督说明](../learning/oscomp-compat-baseline.md#本次正式容器结果)。
评测同时运行主机正确性回归；本次原分不作为无干扰吞吐或时延对比。

容器首跑在原Harness的编译阶段失败：实际GCC13.2不接受-mno-lsx/-mno-lasx，
Job为Compile Error且没有串口。official入口在采集串口前分类该状态。镜像构建
显式使用LP64S/soft-float并禁自动向量化；主机GCC15默认参数保持，手写SIMD
保存汇编仍参与构建。构建环境写入identity，不能用Docker返回0推断编译成功。

原LA basic两套目录的brk ELF完全相同，SHA-256
`d3882df3c12108f783d23be0db1eb66429f750686d54ac1151f545067f0c9310`，
PT_INTERP为`/lib64/ld-linux-loongarch-lp64d.so.1`，另需现有`/lib`与`/usr/lib64`
路径。固定Linux同ELF缺链接退出127、补原盘loader链接退出0；bootstrap发布
三个链接，原盘程序与runtime不变。第二次容器已编译并启动两侧，发现此准备错误后
主动结束，资源回收未验证；该流不作为正式总分，也不拼接后续运行。

原LA musl的cyclictest、iozone、netperf/netserver及entry-dynamic.exe另请求
`/lib64/ld-musl-loongarch-lp64d.so.1`。bootstrap必须把它指向原盘
`/musl/lib/libc.so`，不能用只有`/lib/ld-musl-loongarch64.so.1`的路径代替。
同一未修改ELF在固定Linux16KiB与BoarOS的两种RAM下，缺链接均exec ENOENT，
补原盘链接后help/version入口正常，native状态与Linux一致且根owner正常收口。
这是兼容启动环境修复，通用内核没有放宽exec规则；1915历史成绩不因此被改写。
显式shell后原libc-test列表已在RV/LA自然结束，musl均217；LA的glibc179成功/
38失败与固定Linux逐ID状态一致，原输入的共同失败保持。原iozone/netperf两种
libc均得到诊断分数和正常回收，netperf的可选socket项错误仍保留。源码、环境、
失败目录及重建命令见[原组后续诊断](../learning/oscomp-compat-baseline.md#启动修复后的原组诊断)。
RV多出的静态clock_gettime失败已定位为原glibc的malloc初始化在main前写入
EAGAIN；时钟实际返回0。独立诊断校验原ELF SHA后只读观察默认路径，并用公开的
Linux返回故障profile验证同一因果链，512MiB/1GiB都正常回收。参见
[首次errno写入](../learning/oscomp-compat-baseline.md#rv静态clock_gettime的errno来源)。

## 整体审查后的报告契约

一次独立只读整体审查确认四项P2并集中修复：显式公共INIT_CONFIG覆盖命令行/环境
架构配置；本地diagnostic剔除调用者的INIT_CONFIG与MAKEFLAGS等继承覆盖，显式
选择并记录本次实际评测配置；官方生命周期不从COMPLETE推导；单项EXEC/WAIT
错误进入结构化结果。源码和同ELF程序未因报告修复被改写。

completed仅描述原组外层END/退出0，另存cases/observed_errors与原judge分数。
LTP逐项保存原shell退出值；native wait状态只从实际TIMEOUT-END读取，其他情况
保持null。TFAIL/TBROK、TCONF、源码helper SKIP、人工EXCLUDE、监督超时、EXEC
装载失败、WAIT运行错误分别保留；已报告的失败不能被后续skip或退出0掩盖。
不完整单项仍incomplete，非零但无法证明发生阶段的记录保持unknown；不猜main已到达。
识别实际LTP彩色标签时只跳过metadata中的ANSI SGR，证据原字节和原judge输入不变。

官方只读observer验证本次容器digest与隔离submit挂载，读取QEMU实际PID/start_ticks、
存活与消失。PID复用/重复实例是observer错误。进程存活超过预算并留1秒测量余量
才记录budget_observed；COMPLETE只表示脚本完成。未观察到真实进程结束或预算
事实时exit_reason为unknown-lifecycle，不能给当前组补记timeout或正常回收。
原Harness不提供可靠逐架构QEMU returncode，该字段明确null；自然结束仍需真实
PID1/页/堆/任务栈/根盘/设备收尾。容器清理由本次cidfile标识，不能仅凭名称误删其他owner。

若修正报告采集器，`python3 -B tests/oscomp/official.py --collect-existing build/某次运行`
只重读同一冻结identity、原Job、两份实际串口与observer，不再次启动或构建，不合并
其他运行。核对原联合postwork整数分并记录collector commit/tree及dirty状态；冻结
内核源码身份与collector身份分开。单独观察运行中的已授权容器可用--observe-only。
第三次运行固定722b517，在RV的原fs_fill遇到ext4_bcache_free的NULL+0x38 fatal；
现场没有原调用栈，且最终Job/LA生命周期未收齐，不作为完成的正式基线。
main的a15fbf3已用实际lwext4冷缓存树读取OOM独立复现同一释放位置并修复，
OOM/读失败、重试/卸载/堆清零、完整host恢复及双侧真实程序回归通过；
先前事件与这条调用链的唯一归因仍须区别于确定性复现，干净官方全量重跑继续。

实际kernel fatal优先于进程结束或预算证据，标记kernel-runtime-error，保留原故障行；
当前组/单项保留kernel owner，未到达组保持not-reached，没有伪造exit/wait状态。
原judge的Accepted与整数分只证明原判分完成，results_captured与baseline_established
分开。两侧均有正常PID1/owner收口，或真实总预算证据，且没有运行阻塞时才建立基线；
否则官方入口返回失败并保留完整已取得结果。总预算终止不声明正常资源回收。

该轮聚焦与真实回归已包括host反例、两侧supervisor、原环境basic/BusyBox自然退出、
RV完整架构、LA核心/fatal/dynamic、RV真实userland、GNU五形态、双方1366ABI、
栈、LA PCI/reset/RNG/net/root-I/O及双方TTY/PTY。一次并行VFS15秒超时后，同构建
独立及完整较低并发重跑通过，未证明历史触发归因。TTY的固定Linux LA1GiB同步
反例则已由前台子shell READY修正并先落main。
