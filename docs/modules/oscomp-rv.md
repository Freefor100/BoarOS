# RV 官方测例运行与评分

本模块仅存在于 `oscomp-rv-compat`。通用能力来自 main；本分支独立保留
uname 4.15.0、启动配置和评测入口，不整体回合主线。这里只跑 RV，不能当作双架构比赛交付。

## 固定输入

`tests/oscomp/inputs.json` 固定 `pre-20250615` 发布镜像及压缩包的 SHA-256，
以及 `references/oscomp-autotest` commit
`d1bb3a3c4b27274e196a2648518525c1a304e339`。四个发布资产长期保存于该目录，
缺失或校验失败直接报错，不重新下载、不修改原盘。LA 只校验身份，不启动。
来源为清单记录的 GitHub release 与 Harness；运行依赖包括项目工具链、QEMU、
e2fsprogs、Python 3.11+、jinja2、pytz，原 judge 使用宿主 Python。

## 启动和用户态环境

`init.json` 将 PID 1 配成原盘 `/musl/busybox sh /boaros-start.sh`。
runner 从原盘建立可丢弃副本，只增加自己的启动脚本。原测试脚本、二进制、权限及
判断逻辑均保留。内核没有比赛路径或调度特判。

启动脚本通过原 BusyBox 创建 `/bin /lib /tmp /dev /proc`、工具与加载器符号链接，
通过 mknodat 创建 null/zero/console，将标准 fd 重新绑定到真实 console 节点，
再挂载内核 procfs。`/tmp` 是 ext4 目录，不是 tmpfs。
每组在自己的 libc 根目录执行原 `*_testcode.sh`，分别设置 `LD_LIBRARY_PATH`，
避免同时搜索两套 libc。musl 的普通/sf 加载器名指向镜像自带 libc；
glibc 加载器指向其真实文件。proc 内容来自真实内核对象，没有假随机设备或测试输出。
原镜像两侧的 `hello` 辅助脚本都写死执行 `/code/lmbench_src/bin/build/lmbench_all`；
进入 lmbench 组前，用户态只将该路径链接到当前 libc 目录下镜像自带的真实二进制，
不改动 `hello` 或组脚本。

默认顺序为 basic、busybox、cyclictest、iozone、iperf、libcbench、libctest、
lmbench、ltp、lua、netperf，每组先 glibc 后 musl。无逐组超时、重启或失败后宿主拼接。

## 重建与评分

```sh
python3 -B tests/oscomp/run.py --output build/oscomp-rv-baseline
# 明确标为诊断，不能合入正式总分：
python3 -B tests/oscomp/run.py --output build/oscomp-rv-diagnostic --diagnostic-timeout 60
make all                         # 恢复默认 /init 配置
make test-init-config-riscv       # 交替重建检查，结束后恢复默认
```

runner 读取固定 Harness `kernel/judge/config.json`。其中 `qemu.timeout=3600`；
60 秒只是 `run_qemu.py` 的缺省值，不能冒充随附配置。RV 参数按该源码取 `smp`、
`mem`（默认 1 hart/1G）、VirtIO block/net、user net、RTC UTC、OpenSBI default。
本 profile 无第二盘，不伪造不存在的输入；总时间预算覆盖一次 QEMU 启动。

原 `parse_serial_out_new` 和 22 个 judge 原样运行，未到达组也由原 judge 产生结果。
原 `postwork.postwork` 接收仅含 RV 的 summary；LA 未运行，不在输入中伪造成绩。
其整数分数为 RV 投影，保留原始分组分数与 LTP 变换，不自行重写总分公式。
每组状态区分未到达、超时、脚本失败和已结束；已结束不意味着该组所有测试通过。

`report.json` 保存提交/脏状态、内核和输入哈希、配置、QEMU 命令/版本、一次启动计数、
退出原因、22 组细目及原 postwork 分数。`judge.json` 为原解析器结果，`serial.log`
为完整串口，`identity.json` 在启动前写入，方便中断后定位。HTML 是原 postwork 展示，
可能带有 LA 空列，不能把它解释成跑过 LA。输出只允许放在新建的 `build/` 子目录。

## 证据边界与清理

历史逐组重启、修改启动方式的成绩仅作诊断；官方脚本是否启动、环境缺口和内核 ABI
错误分别记录。镜像 basic 的 `run-all.sh` 为 0644，原包装器却直接执行它；本入口
保留该行为，不能据此把 basic 的低分全部归为内核缺少对应 syscall。

评分核对后将结论、运行身份和重建命令写入本文，运行产物由 `make prune-build`
清理。原始 `.img/.img.xz` 在 references，不属于清理范围。通用新缺陷先最小复现，
回 main 修复并验收，再 merge 回本分支重新构建运行。

## 2026-09-29 单次启动 RV 正式基线

干净的 `f730e70b9fd9d4d4cd1b38bde9ce4ef211e5059a` 合入已验收主线后，以
`python3 -B tests/oscomp/run.py --output build/oscomp-proc-final-20260929`
从原 RV 镜像的可丢弃副本启动一次。原解析器、22 个 judge 和 postwork 未改动，
**RV 投影整数分数为 662**，不是双架构成绩。QEMU 11.1.1 运行 3600.0037 秒后
达到原配置的 3600 秒总预算；没有逐组重启或拼接诊断成绩。

| 组 | glibc 原分数 | musl 原分数 | 实际范围 |
|---|---:|---:|---|
| basic | 0 | 0 | 两侧脚本结束；原 `run-all.sh` 权限阻塞仍在 |
| busybox | 52 | 52 | 两侧脚本结束；部分子项失败 |
| cyclictest | 0 | 0 | 两侧脚本结束；调度接口等内部子项失败 |
| iozone | 0 | 0 | 两侧脚本结束；吞吐子项 `shmget` 仍失败 |
| iperf | 0 | 0 | 两侧脚本结束；随机设备/daemon 路径仍有缺口 |
| libcbench | 37.3857471247465 | 30.541587541110484 | 两侧结束并计时 |
| libctest | 174 | 215 | 两侧原脚本结束，含通过及失败项 |
| lmbench | 50.6529945999422 | 50.82701976762104 | 两侧结束；旧 `/code/.../lmbench_all` 缺失报错为 0 |
| ltp | 0 | 0 | glibc 运行到第 107 个案例时超时；musl 未到达 |
| lua | 0 | 0 | 两侧未到达 |
| netperf | 0 | 0 | 两侧未到达 |

glibc LTP 中有 53 次 `tst_memutils.c:94` 读取 `/proc/meminfo` 转换失败。
从原 `sdcard-rv.img` 提取的 `/glibc/ltp/testcases/bin/abort01` SHA-256 为
`f6b658f4e37b187a10022a3202af1458cbb1344e8edccd075c1312e066ceabb7`；
其符号 `tst_available_mem` 在固定二进制中先可选读取 `MemAvailable`，失败后
于源码行 93、94 必需读取 `MemFree`、`Cached`。当前 proc 只提供前者，故
本次行 94 的直接缺口是 `Cached`；后续应从真实文件页/内存统计定义该字段，
不能用假零通过检查。`cgroup_fj_function.sh` 另报 `setpgid` 未实现及控制器缺失；
随后 `cgroup_fj_proc` 无新串口输出直到总预算结束，其等待根因尚未定位。
这些 LTP 故障不能统称为 procfs 失败，也不说明未到达的组有实现缺陷。

上次 663 分与本次 662 分的离散子项差异是 glibc `libctest static utime`
由通过变为失败；动态 `utime` 两次都失败。原测试在 `UTIME_NOW` 后要求
`fstat` 时间不早于 `time(0)`，本次未满足，时间/文件元数据边界待同一 ELF
最小复现。libcbench/lmbench 浮点分数也有运行波动；一分差异不能直接归于
本次 proc 改动。两侧 lmbench 完成原脚本，没有旧绝对路径错误，但仍不等于
全部内部功能正确。

运行身份：内核 SHA-256
`4b95738eb76c2b1d0577658377667c94233116f0282e48e42db5e4eb25da0a9b`，
PID 1 配置 `532c958496d1e18bc72623b0eef3905d67b1c7b31a574b25eefb47342034d44a`，
启动脚本 `1bd689861a5d5125a68038e1218f44e078be8e97bf528975bafbeb8f393440d8`，
Harness 配置 `082a086816477687be6cd175ae0eee3cbaf466892fc2ff509dfeeff3cebf3813`，
串口 `d9ec545b0d0ab9a40c7e3b21f07b244a0bab27b4ce6a668c7b4c8a437151604e`。
原 RV 镜像 SHA-256 为
`f419468678d342133546add2f8459ea09aeba987ba968e28753d6ee656996b8b`；
LA 及两份压缩包也由 [inputs.json](../../tests/oscomp/inputs.json) 校验但未运行。
QEMU 参数、22 项状态、原 judge 明细与启动时空的 Git 脏状态由上述命令的
`report.json`/`judge.json` 重建；清理后不把旧 `build/` 路径当永久证据。

## 2026-09-28 proc 挂载限时诊断

在新 proc 挂载配置上先用 `python3 -B tests/oscomp/run.py --output build/oscomp-proc-diagnostic-20260928 --diagnostic-timeout 120` 做**限时诊断**：一次启动在总预算 120 秒处终止，停在 glibc iozone，后续组未到达。原 judge 在本次诊断给 busybox 两侧各 52 项、总整数 104；这不是正式分数。启动脚本成功挂载真实 proc，`ps` 列出进程；原脚本将 df/free 标 success，但 df 只有表头，glibc free 出现溢出的使用量、musl free 全零，内容不能当作正确统计。该次内核 SHA-256 `0bf2dcab32a4bd7d4adf57c99a6d503cfe47593f8f9ffd87f48c937d33b3430f`，脚本 SHA-256 `3b4d48bbadd33596f700b590183bcb992ae28647bbd844e9c7094159cdd2a880`，串口 SHA-256 `a62a42d2144af5380bf281cdbd5d19e7e18ae1219cb05bd06e78021594809047`；运行时脚本和本文尚未提交，报告明示工作区脏状态。正式预算必须在提交后另开一次启动，不能拼接诊断结果。

## 2026-09-28 proc 挂载首轮正式评分

干净的 `d0b6530fdd2e6a4f4461dd835bac5f0a25ac5dde` 在单次 3600 秒 RV
运行中由原 postwork 得 **663 分**；QEMU 因总预算终止。22 项原 judge 分数按
glibc/musl 顺序分别为：basic 0/0、busybox 52/52、cyclictest 0/0、
iozone 0/0、iperf 0/0、libcbench 37.455975479567265/30.566004736104、
libctest 175/215、lmbench 50.79791483884551/50.870992196784016、
ltp 0/0、lua 0/0、netperf 0/0。glibc LTP 执行到第 107 个案例
`cgroup_fj_proc` 后无新串口输出，标为超时；musl LTP 与两侧 Lua/netperf
未到达，不可视作已执行失败。原 libctest 此次已启动且有通过及失败案例，
旧的“脚本 not found”前置阻塞已解除。

首轮启动环境尚未满足原镜像 `hello` 的绝对 `/code/.../lmbench_all` 路径：
lmbench 期间反复打印该目标不存在，却仍产生计时分数。原盘 `/glibc/hello`
和 `/musl/hello` 内容相同；后继提交仅按 libc 建立指向镜像二进制的链接，
因此首轮分数保留为有明确环境缺口的历史运行，不能与后继运行拼接。
本次内核 SHA-256 `0bf2dcab32a4bd7d4adf57c99a6d503cfe47593f8f9ffd87f48c937d33b3430f`，
启动脚本 SHA-256 `3b4d48bbadd33596f700b590183bcb992ae28647bbd844e9c7094159cdd2a880`，
串口 SHA-256 `82ca550dff6c73427c8579d10fd60e49cebb5e5b85c7984973218444f50e5e70`。
固定输入及 QEMU 配置同下述旧基线身份；启动时工作区干净。重建命令为
`python3 -B tests/oscomp/run.py --output build/oscomp-proc-formal-20260928`，
需检出首轮提交而不是使用当前已修正的启动脚本。

## 2026-09-28 单次启动旧基线

**原 postwork 整数分数 267，未取整合计 267.2644974509601。** 这是固定 Harness
的 RV 投影，不是双架构总成绩，也不是满分 267。一次启动自北京时间 13:33:40
运行 3600.007 秒，由总预算超时终止；没有逐组重启、追加时间或拼接诊断成绩。

| 组 | glibc 原分数 | musl 原分数 | 本次状态 |
|---|---:|---:|---|
| basic | 0 | 0 | 两侧包装脚本结束，内部 run-all.sh 权限失败 |
| busybox | 49 | 49 | 两侧结束，各 49/55 项计分 |
| cyclictest | 0.0 | 0.0 | 两侧结束，内部子项失败 |
| iozone | 0.0 | 0.0 | 两侧结束；自动模式有结果，计分的吞吐子项失败 |
| iperf | 0.0 | 0.0 | 两侧结束，内部子项失败 |
| libcbench | 37.40009732614475 | 30.679368580220913 | 两侧结束并计分 |
| libctest | 0 | 0 | 两侧包装脚本结束，内部脚本启动失败 |
| lmbench | 50.44911570853501 | 50.735915836059405 | 两侧结束并计分，包含内部程序错误 |
| ltp | 0 | 0 | glibc 运行中耗尽总预算；musl 未到达 |
| lua | 0 | 0 | 两侧未到达 |
| netperf | 0.0 | 0.0 | 两侧未到达 |

已结束组的包装脚本退出码均为 0；这不能代替组内成功判定。glibc LTP 无 END 标记，
但原解析器会在 EOF 关闭并读取该 judge，并非把所有部分日志丢弃。原 LTP judge
得到 105 条案例记录、分数为 0；它按 Summary 的 passed 字段计分，旧式单行 TPASS
和退出码 0 本身不增加分数。这里保留原规则，不自行纠正或补分。LA 未运行；原
postwork 生成的 LA 零值展示列是其默认输出，不代表进行过 LA 验证。

### 运行身份与重建

- 内核提交：`635a12ae6a135abff83a6ecd68c7d82f2f0622ac`。
- 内核 SHA-256：`d53b52fa56b943f5bdc784471a362f1b4b037c8915a422cf709feb67c5f02916`。
- Harness 配置 SHA-256：`082a086816477687be6cd175ae0eee3cbaf466892fc2ff509dfeeff3cebf3813`。
- PID 1 配置 SHA-256：`532c958496d1e18bc72623b0eef3905d67b1c7b31a574b25eefb47342034d44a`。
- 启动脚本 SHA-256：`1327c7efa5ac4d0c8302d8b07d9fbd65d9b22e357649c02b2725c08094ce38aa`。
- 本次串口 SHA-256：`b605862d1758716f0bb8e9d9aa53f35903d43294543e57e038b38aac8589ba3e`。
- 构建/启动时工作区干净；此后只合入文档收口，没有修改运行中的代码。
- QEMU 11.1.1，RV64 virt，1 hart，1 GiB，OpenSBI default；固定输入及四个资产哈希见
  [inputs.json](../../tests/oscomp/inputs.json)。额外磁盘为无，原镜像只在运行副本上增加启动脚本。
- 默认/自定义 PID 1 交替测试后恢复评测配置，重新生成的二进制哈希与本次启动一致。

在上述提交或代码相同的文档后继提交，执行本模块的 `run.py` 命令即可重建过程。
运行目录在核对后清理，不能把历史 build 路径当永久证据；性能浮点分数会随宿主负载
变化，不要求再次运行逐位相同。本次 QEMU 参数为：

```sh
qemu-system-riscv64 -machine virt -kernel kernel-rv -m 1G -nographic -smp 1 \
  -bios default -drive file=build/oscomp-rv-baseline/root.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 -no-reboot \
  -device virtio-net-device,netdev=net -netdev user,id=net -rtc base=utc
```

### 已定位的阻塞与待调查现象

- **basic**：原盘两套 `basic/run-all.sh` 的 mode 为 0644，包装器直接 `./run-all.sh`，
  得到 Permission denied。保留原权限；不能将 0 分解释为所有 basic syscall 均失败。
- **BusyBox**：两侧未计分项为 df、dmesg、ps、free、hwclock、kill 10。
  proc/设备及日志接口缺口和具体调用结果应分别定位，不用一个泛化原因覆盖全部。
- **cyclictest**：可见 sched_getaffinity ENOSYS、无法取得 scheduler 参数；hackbench
  创建 fdpair 也失败。调度和相关接口仍属后续能力。
- **iozone**：两侧自动 4 MiB 模式均完成；短预算超时不是永久卡死证据。
  吞吐子项明确在 shmget 返回 ENOSYS（38）后结束。原 judge 只提取吞吐部分的
  Max throughput per process，因此自动模式的成功输出不计分。慢写和异常日期
  仍待最小复现，不据此直接选择性能或时钟修复方案。
- **iperf**：缺少 `/dev/urandom`；glibc 服务器另报 daemon 化 ENOSYS。尚未证明
  消除这些前置阻塞后网络测例能通过，不能直接归咎于 TCP 数据路径。
- **libctest**：两套 run-static.sh/run-dynamic.sh 均报 not found。文件实际存在，
  无 shebang；原 BusyBox v1.33.1 的 ENOEXEC 回退会使用 `/proc/self/exe`。
  同原盘、同内核的独立诊断中，向可执行文本写入 `echo NO-SHEBANG-RAN` 后直接
  执行为 127，显式 `/musl/busybox sh 文件` 为 0。这项诊断不计分；不以假 proc
  文件或修改上游脚本绕过。固定源码依据见[ELF 学习](../learning/elf-loading.md#shebang-与-shell-回退2026-09-28)。
- **lmbench**：虽有计分，仍有 `/tmp/hello` 启动错误；计时输出不是全部功能正确的证明。
- **LTP**：许多案例首先缺 `/proc/meminfo`。最终进入 cgroup_fj_function.sh 后反复
  `cut: /proc/5/stat: No such file or directory`，直至总预算结束。musl LTP、Lua 和
  netperf 没有启动，不能根据本次 0 分判断这些程序的独立通过率。

后续仍按 goals 的真实 procfs/设备与 mount 生命周期设计推进，再补 tmpfs、运行环境
接口并重新评测；SysV IPC 待设计。这里不把原 judge 的分数直接等同于内核能力覆盖率。
