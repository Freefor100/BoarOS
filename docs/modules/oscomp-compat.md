# 双架构原评测运行与评分

本模块仅存在于oscomp-compat。main保留通用内核和自身身份；通用修复先落main再
单向合入此分支。本分支保留兼容uname4.15.0、原盘启动配置、监督与判分。分支
由oscomp-rv-compat本地改名，旧远程引用没有迁移。历史RV专项记录仍保留原身份。

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

默认`make all`同时生成kernel-rv/kernel-la。RV/LA分别生成整数raw supervisor、
init.json和头文件；`INIT_CONFIG_RV`/`INIT_CONFIG_LA`独立覆盖，显式共同
`INIT_CONFIG=config/init.json`同时恢复通用fixture。构建中不可让另一组不同配置
覆盖正在冻结的kernel；先构建、复制内核及身份，再运行。

## 原盘bootstrap与监督

PID1直接exec原盘`/musl/busybox`，bootstrap作为sh -c参数编入内核，不由宿主
向原盘注入helper。启动创建/bin、设备、proc和/dev/shm，并从目标profile建立
原loader链接。LA除/lib外还需/usr/lib64/ld-linux-loongarch-lp64d.so.1；RV的
普通/sf musl名称保留。两个libc的LD_LIBRARY_PATH和工作目录分别设置。

默认组顺序为basic、busybox、cyclictest、iozone、iperf、libcbench、libctest、
lmbench、lua、netperf、ltp，各组先glibc再musl。basic/run-all.sh的原发布权限
为0644，bootstrap只补执行位；lmbench的写死辅助路径指向原盘当前libc二进制。
LTPROOT和其testcases/bin在同名Lua test.sh之前，保留原比赛目录遍历与参数。

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
官方容器基线与完整回归结果仍须本阶段后续运行，不能从现有bootstrap验收推导。
