# RV 官方测例运行与评分

本模块仅存在于 `oscomp-rv-compat`。通用能力来自 main；本分支独立保留
uname 4.15.0、启动配置和评测入口，不整体回合主线。这里只跑 RV，不能当作双架构比赛交付。

## 固定输入

`tests/oscomp/inputs.json` 固定 `pre-20250615` 发布镜像及压缩包的 SHA-256，
以及 `references/oscomp-autotest` commit
`d1bb3a3c4b27274e196a2648518525c1a304e339`。四个发布资产长期保存于该目录，
缺失直接报错，不重新下载、不修改原盘。常规运行复用已核对的发布输入，不再扫描
RV/LA 四个大文件；需要重新校验时显式传 `--verify-inputs`。LA 不启动。
来源为清单记录的 GitHub release 与 Harness；运行依赖包括项目工具链、QEMU、
Python 3.11+、jinja2、pytz，原 judge 使用宿主 Python。

## 启动和用户态环境

本分支普通 `make all` 将 `tests/oscomp/init.sh` 和通用单项监督程序编入生成的
PID 1 配置，直接启动官方原盘的 `/musl/busybox`。不需要本地 runner 向测试盘注入
启动文件，也不依赖额外 libc 或修改原测例二进制。生成配置保存在忽略的 build；
`INIT_CONFIG=...` 仍可显式选择其他启动配置。内核没有比赛路径或调度特判。
使用只含 `/init` 的通用模块 fixture 时，需显式传入 `INIT_CONFIG=config/init.json`；
默认评测启动配置要求官方盘中的 BusyBox 和目录布局。

启动脚本为两侧 `basic/run-all.sh` 补执行权限，使原包装器能够调用它。

启动脚本通过原 BusyBox 创建 `/bin /lib /tmp /dev /proc`、工具与加载器符号链接，
通过 mknodat 创建 null/zero、串口、random/urandom、RTC 和根盘设备节点，将标准 fd
重新绑定到真实 console，再挂载 procfs 与 `/dev/shm` 的 tmpfs。`/tmp` 仍为 ext4，
权限为1777。随机节点接入真实内核随机源；节点存在不等于可信熵已就绪，本 profile
不提供 VirtIO RNG，不能用固定字节或成功存根掩盖缺失的熵源。
每组在自己的 libc 根目录执行原 `*_testcode.sh`，分别设置 `LD_LIBRARY_PATH`，
避免同时搜索两套 libc。musl 的普通/sf 加载器名指向镜像自带 libc；
glibc 加载器指向其真实文件。proc 内容来自真实内核对象，没有假随机设备或测试输出。
LTP 阶段设置实际 `LTPROOT`，其 `testcases/bin` 排在 libc 根目录之前；否则 `. test.sh`
会错误加载 Lua 的同名驱动。其他组保留自己的根目录查找顺序。
原镜像两侧的 `hello` 辅助脚本都写死执行 `/code/lmbench_src/bin/build/lmbench_all`；
进入 lmbench 组前，用户态只将该路径链接到当前 libc 目录下镜像自带的真实二进制，
不改动 `hello` 或组脚本。

默认顺序为 basic、busybox、cyclictest、iozone、iperf、libcbench、libctest、
lmbench、lua、netperf、ltp，每组先 glibc 后 musl。各组串行执行原脚本，不切换 LTP 的
上游 runtest 清单，不替换上游runtest，也不在失败后重启、拼接结果。

LTP 仍使用比赛脚本的原目录遍历、原参数及原 START/RUN/FAIL/END 标记。
`ltp-hook.sh` 只在临时脚本副本的唯一单项执行行接入 `case`，原盘文件保留。
监督程序使用原 BusyBox 的 exec/文本回退，继承 cwd 和环境，将单项置于独立进程组。
直接子进程不是组长，仍可调用 setsid；监督者在握手放行前离开测试组，避免测例的
`kill(0, signal)` 终止原包装器。监督者的存活 PID 保护组身份，不使用按名称的进程扫描。
正常退出和信号结果原样转换为 shell 状态；超时先发 TERM，
两秒后必要时发 KILL，另保留 native wait status，返回 124 而非成功。
超时结束记录直接 child PID、测试 PGID、预算到达与回收完成的经过毫秒数；
时钟不可用时明确标记 unavailable。这些是包含等待的经过时间，不是 CPU 时间。
直接子进程回收后，清理仍留在该组中的后代；不会向等待信号的程序伪造启动信号，
也不承诺回收已脱离测试组的守护进程。官方脚本本身将辅助程序作为无参数单项执行，
监督不能把这种输入转化成上游控制脚本的有效输入或语义通过。

直接无参数运行时缺少配套控制器或输入、不能独立结束的辅助程序由 `tests/oscomp/ltp-skips.tsv` 明确列出来源和原因；固定依据为pre-2025树中的ltp-full-20240524。原遍历仍输出RUN/FAIL，跳过另记SKIP并返回125，不输出伪造Summary、TCONF或通过结果，原judge不给通过分。控制脚本和普通测例照常运行，不能将cgroup能力缺失本身作为跳过所有相关测例的理由。有限的大规模压力负载不因耗时长自动归入该清单。

默认每项 LTP 安全预算300秒，可在构建时用 `OSCOMP_CASE_TIMEOUT` 设置，0关闭此预算。
这属于本项目的执行预算，可能提前结束合法长测试；超时不代表内核缺功能，完成循环
也不代表语义通过。官方总启动预算和原 judge 不变，缺少 `kernel-la` 仍阻塞完整 Harness。

## LTP 单项归因

比赛脚本无参数遍历 `testcases/bin`，与上游按 `runtest` 选择程序、参数及控制环境的
执行不同。固定依据为 `references/oscomp-testsuits` 的 pre-2025 树中
`ltp-full-20240524/runtest/mm`、`doc/users/quick_start.rst` 和各项源码。
记录单项时分开保存执行角色、首个阻塞、实际是否到达目标接口、原 wait/输出断言与
后续依赖；先归因，再决定适配或修复。运行快照留在忽略的 build，不追加到本文。

| 类别 | 判定与处理 |
|---|---|
| 控制器或输入辅助程序 | 必须有上游父调用、停止信号或输入协议的源码依据。仅直接无参数调用可跳过125；父测试继续执行，不能将子系统缺失整体划入跳过表。 |
| 上游已知测例缺陷 | LTP 20240524 的 `runtest/mm` 明确因无限循环禁用 `shmat1`；它的24小时alarm成功退出不证明线程工作完成。这类项单独记录，可显式诊断排除，不能靠调度特判迎合用户态握手。 |
| 有限大工作量 | `shm_test` 默认30线程、1000轮，上游清单用 `-l 10 -t 2`；`mmap1` 的5000轮是有效竞争检查。次数或耗时不构成测例错误，预算排除与能力失败必须分开。 |
| 功能或支持子集缺口 | 公共tmpdir准备的 `chown` 失败可能发生在目标接口之前。区分未分派系统调用、具体clock/flag/fcntl缺口和已实现路径错误；补齐首依赖不等于后续全部通过。 |
| 输入、环境与参考差异 | 缺工具、scratch设备、账户或可信熵，编译时未启用libnuma/libaio，架构/版本筛选和原libc包装行为分别核对。运行时补功能不能启用已编译掉的测试；原ELF和原judge保留，不用假配置或空成功。 |
| 已实现路径的异常 | 对照匹配的Linux源码或同ELF窄探针，定位错误优先级、内容、并发和owner；未知原因保留待定位，不通过跳过隐藏。旧LTP的errno期望也须与固定参考版本核对。 |

`FAIL LTP CASE ... : 0` 是原脚本固定标记，不表示测例断言全部失败或全部成功；
部分旧程序有TFAIL仍退出0，部分helper没有有效断言也退出0。Summary、实际工作和
资源回收与退出状态共同构成结论。未到达项和原watchdog终止项都不能补记通过。

## 重建与评分

官方 Docker 入口见 `references/oscomp-autotest/README.md`，它直接调用 `make all`
并启动两种架构。下列 `run.py` 是复用原脚本与 judge 的本地 RV 投影，不代替该入口。

```sh
make all                         # 官方构建入口；无需修改官方测试盘
python3 -B tests/oscomp/run.py --output build/oscomp-rv-baseline
python3 -B tests/oscomp/run.py --groups environment --output build/oscomp-rv-environment
python3 -B tests/oscomp/run.py --groups benchmarks --output build/oscomp-rv-benchmarks
# 聚焦验证仍执行完整原 LTP 包装器，不选内部清单：
python3 -B tests/oscomp/run.py --groups ltp --output build/oscomp-ltp
python3 -B tests/oscomp/test_official.py
# 明确标为诊断，不能合入正式总分：
python3 -B tests/oscomp/run.py --output build/oscomp-rv-diagnostic --diagnostic-timeout 60
# 观察原生完成或阻塞位置，关闭总预算和逐项监督期限：
python3 -B tests/oscomp/run.py --output build/oscomp-rv-uncapped --diagnostic-timeout 0 --case-timeout 0
make all                         # 恢复完整官方启动选择
make all INIT_CONFIG=config/init.json # 显式恢复通用 /init 配置
make test-init-config-riscv       # 交替重建检查，结束后恢复默认
```

runner 读取固定 Harness `kernel/judge/config.json`。其中 `qemu.timeout=3600`；
60 秒只是 `run_qemu.py` 的缺省值，不能冒充随附配置。RV 参数按该源码取 `qemu.smp`、
`qemu.mem`（默认 1 hart/1G）、VirtIO block/net、user net、RTC UTC、OpenSBI default。
本 profile 无第二盘，不伪造不存在的输入；总时间预算覆盖一次 QEMU 启动。
`--diagnostic-timeout 0` 将宿主监督设为无截止时间，`--case-timeout 0` 关闭客体逐项监督期限；两者不会改动测例自身的 watchdog 或原 judge。此模式用于定位长任务与真实阻塞，结果明确标为诊断，不能替代官方固定预算成绩。

`--diagnostic-exclude CASE` 可显式排除一个 LTP 文件名，重复参数可列多个；默认清单为空。对应构建参数为 `OSCOMP_DIAGNOSTIC_EXCLUDE`。原目录仍遍历该条目，另输出 `EXCLUDE` 原因并返回125，不输出Summary或通过分；报告单独保存排除清单，并将整轮标为诊断。有限压力项的人工排除不能归为缺少控制器，也不能宣称它已执行或通过。不同启动的日志和分数不拼接。

原 `parse_serial_out_new` 和 22 个 judge 原样运行，未到达组也由原 judge 产生结果。
原 `postwork.postwork` 接收仅含 RV 的 summary；LA 未运行，不在输入中伪造成绩。
其整数分数为 RV 投影，保留原始分组分数与 LTP 变换，不自行重写总分公式。
每组状态区分未到达、超时、脚本失败和已结束；已结束不意味着该组所有测试通过。

`report.json` 保存提交/脏状态、内核和输入哈希、配置、QEMU 命令/版本、一次启动计数、
退出原因、22 组细目及原 postwork 分数。`judge.json` 为原解析器结果，`serial.log`
为完整串口，`identity.json` 在启动前写入，方便中断后定位。HTML 是原 postwork 展示，
可能带有 LA 空列，不能把它解释成跑过 LA。输出只允许放在新建的 `build/` 子目录。
`release_assets_verified_this_run` 区分此次全量复查与复用既有输入；`--groups benchmarks`
仅执行五项原脚本，未选组只由原 parser 产生空结果。最新实测、cleanup 错误和
cyclictest 零采样见[五项评分记录](../learning/data-path-budget-experiments.md#受控延迟与原版五项评分补测2026-10-06)。

## 证据边界与清理

历史逐组重启、修改启动方式的成绩仅作诊断；官方脚本是否启动、环境缺口和内核 ABI
错误分别记录。镜像 basic 的 `run-all.sh` 为0644，启动环境负责补执行权限；保留测例
内容与原包装器调用方式，不把环境准备问题归为缺少对应 syscall。

本文维护启动、监督和评分契约，不保存逐次成绩表或运行快照。输出留在忽略的build；
正在调查、人工中断及关联补跑的原始日志/镜像必须保留，核对并确认不再需要后才清理。原始 `.img/.img.xz` 在 references，不属于清理范围。通用新缺陷先最小复现，
回 main 修复并验收，再 merge 回本分支重新构建运行。

## 原测例、判分标签与环境

basic的mount/umount原源码要求vfat和`/dev/vda2`；本profile只有原根盘，后端没有
vfat。程序断言失败不能解释成全部mount/umount未实现，也不能用假的分区或成功返回
让它通过。LTP的scratch/loop设备、账户和工具同样要区分真实准备依赖。

原BusyBox命令已使用后台sleep的实际PID，但固定judge仍保留`kill 10`标签；未匹配
得到零分不证明kill失效。保留原judge与原输出，另核对实际命令、状态和被终止子进程。
原iperf UDP的1000G目标是饱和负载；收包量/丢包、TCP的字节吞吐与netperf RR事务
各自解释。内嵌baseline是历史评分输入，不能称为本次匹配配置的Linux性能。

LTP的准备与libc问题见[输入层分析](../learning/user-program-inventory.md#ltp的准备依赖与libc边界)：
公共chown先于目标接口，动态glibc退出/取消可能缺匹配的unwind库，旧musl重试EINTR
或在clone的NULL栈检查前写入。用户SIGSEGV、程序abort和内核fatal必须分开；
不只凭退出码或测例名称归为内核缺陷，更不修改原ELF来提高评分。

## 人工停止与诊断补跑

关闭监督期限不等于存在在线取消入口。当前case监督者只按启动参数处理deadline，
runner的QEMU输入在启动后关闭，不能把宿主QEMU的PID当成客体测例的PID。需要新的
人工干预时先明确是否结束整个启动；不能注入会打印成功的alarm来冒充工作完成。

人为停止保存授权、原因、最后进度与原始现场。即使QEMU因宿主信号返回0，没有客体
完整结束标记仍是未完成；未取得测例自然wait状态就明确记为不可用，不能补成125或通过。
补跑单独保存选择范围、启动配置和输入；新镜像或启动顺序改变前置状态，所以只用于
补充条目调查，不拼接正式总分。普通内核实现保持冻结，改变编译内置启动配置时记录
新的内核身份；两者不能混称完全相同的二进制。
