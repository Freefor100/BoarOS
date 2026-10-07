# 双架构评测的输入、失败与证据

当前路线为 main 单向集成到本地 oscomp-compat；旧远程分支保持原名，
没有远程迁移、push 或托管 CI 运行事实。构建/监督入口见
[兼容模块](../modules/oscomp-compat.md)。

固定 Harness 为 `references/oscomp-autotest` 的
`d1bb3a3c4b27274e196a2648518525c1a304e339`；输入是
`pre-20250615` 原 RV/LA 盘，身份由 `tests/oscomp/inputs.json` 固定。
原 parser、22 个 judge 与 postwork 不改写。官方单 CPU、1 GiB、3600 秒，
监督为既有 300 秒/TERM 后 2 秒 KILL；源码 helper 跳过与人工排除分别记录。

实际容器为
`zhouzhouyi/os-contest@sha256:85dec949df7cef41fd03d30c6ad69f952204540e18d2c62bced9d2e262fef12d`，
工具检查得到 QEMU 10.0.2、两架构 GCC 13.2.0、Python 3.12.3 与 make 4.3。
Dockerfile 的 QEMU 9.2.1 不作为实际运行身份。宿主工具/派生 RTC/RNG 诊断
单独记录；官方不增加 RNG 或替换 QEMU。

## 2026-10-07 的失败处理

| 冻结源码 | 首个已证明阶段 | 结果与处理 |
|---|---|---|
| 54df367 | 容器编译 | GCC13 不支持 `-mno-lsx/-mno-lasx`；没有内核启动或测例结果。采用受支持的整数 ABI 与禁自动向量化构建，手写 CPU 状态汇编保留。 |
| f366e33 | LA 原 GNU ELF 装载准备 | 原 ELF 请求 `/lib64/ld-linux-loongarch-lp64d.so.1`；补指向原盘 loader 的链接，未替换 libc。发现后主动停止该次运行，不拼接分数。 |
| 722b517 | RV 内核运行 | 在原 `fs_fill` 活动期间，`ext4_bcache_free()` 访问 NULL+0x38；没有捕获原调用栈，最终 Job/LA 生命周期也未收齐。不是完成的基线。 |

main `a15fbf3` 的独立实际源代码复现证明：冷缓存 extent 树读取 OOM 后，
非零 `lb_id` 被误当作 buffer 引用，经过 `read_extent_tree_block()` 错误清理
触发同一释放位置。分配失败直接传播后，所有实际分配点/读错误、同 inode
重试、文件内容、正常卸载、堆对象清零与 fsck 通过；完整 host 存储恢复、
RV VFS/files/真实 musl、双方 SQLite DELETE/WAL 与重启、LA 动态 exec/DSO TLS
Linux/BoarOS 两种 RAM 和栈界也通过。兼容分支以 merge commit 集成。
这证明该错误路径已修复，不反推原 fatal 的唯一调用链；新的完整运行仍需要核对。

原 RV `fs_fill` 在固定 Linux
`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e` 下，对同一个 ELF
观察到创建测试盘文件 ENOSPC、TBROK 与 shell status 6，根盘正常收口；
BoarOS 新鲜原盘诊断则在准备阶段返回 ENOSYS、status 6 并正常回收。
这两个结果不是功能通过，测试设备准备与 syscall 缺口仍应分别保留。
该 BoarOS 诊断只在临时内核 ELF 将 UTS 字符串同长替换为
`6.6.0-boaros-dev`，用于进入原 GNU loader；原程序未改，但这一内核身份
不能代替兼容分支 `4.15.0` 的正式运行证据。

## 报告的完成边界

原 Job 的 Accepted 和分数只能证明原判分完成。`results_captured` 表示
完整两侧串口与联合 postwork 被取得并核对；`baseline_established` 还需要两侧
真实进程结束/正常 owner 收口，或实际总预算证据，且没有内核运行阻塞。
fatal 优先于预算分类，当前案例保存 kernel owner 和原故障行，后续未到达组
保持未到达，不补 shell/wait 状态。缺失生命周期事实保持 unknown。

重建命令为 `make test-oscomp-host test-oscomp-official`；官方入口从当前干净
提交快照构建，不读宿主 build 缓存。每次运行独立，禁止拼接不同启动成绩。
若只改报告采集器，`--collect-existing` 只读取同一次原 Job/串口/冻结身份和
observer，分开记录采集器提交与实际内核提交；此操作本身不会补跑未到达的测例。

## 本次正式容器结果

44项原Job分组score的完整精度数据见
[分组成绩表](oscomp-dual-official-results.tsv)。此表从清理前在本会话核对的同次
原Job `rank` 转录，不是重新运行或混用历史RV成绩；最新原串口/Job已被清理，
更细的逐程序输出、终止现场和正常单项耗时不能从这份组分数表逆推出。

冻结的BoarOS提交与最终采集器均为
`11e96a95ff37158be5036fe99cced9bd17cc5575`，tree为
`86885b475407f61104b455b473c19c4492e6cfcb`，采集时工作树干净。
运行使用单CPU、1GiB和每架构3600秒。RV与LA QEMU均由observer以PID/start_ticks
唯一识别，且运行实际超过预算；两份串口、原Job和联合评分已收集。

| 指标 | RV | LA |
|---|---:|---:|
| 分组外层状态 | 20 completed、1 timeout (`ltp-glibc`)、1 not-reached (`ltp-musl`) | 20 completed、1 timeout (`ltp-glibc`)、1 not-reached (`ltp-musl`) |
| LTP-glibc分 | 920 | 908 |
| LTP-musl分 | 0，组未到达 | 0，组未到达 |
| LTP-glibc | 875项状态，27项源码helper跳过 | 1035项状态，27项源码helper跳过 |
| 正常PID1及页/堆/任务栈/根盘/设备收口 | 预算结束，未验证 | 预算结束，未验证 |

只有LTP-musl未到达；前面十类组中的musl均已进入。bootstrap按组顺序执行，
每组先glibc再musl，所以LTP-glibc未结束时不能开始LTP-musl。300秒限制是每个
被监督命令的上限，不是整个LTP-glibc的上限，也不为后续libc预留总预算。
本次采集摘要的两侧 `supervision.timed_out` 均为0：没有完成的TIMEOUT-END记录，
不能用总预算截止反推某个命令越过单项上限，或断言最后命令永久卡住。

`ltp-skips.tsv`与旧兼容提交47cf15a完全一致，34项表的SHA-256仍为
`75b01a673bde74a003d1c92c3fb14476b2fefc55ca8ac79c6c1006208279ad2b`；
本次已走到的区间两侧各命中27项。额外 `diagnostic_exclusions` 是空列表。
静态跳过只涵盖已核实缺控制器/输入的辅助程序，有限但昂贵的压力测试没有因此
自动进入跳过表。现有监督/跳过接入没有丢失，但不能保证两套完整LTP在3600秒跑完。

两侧的 `fs_fill` 均只报告一次TBROK，因准备 `test_dev.img` 返回ENOSYS而未运行主体。
其他错误、skip和ELF loader阶段仍按原串口、shell状态与结构化观察归属；观察项计数
不能等同互不重复的失败测例数。

原Job返回 `Accpted`，分数1915；原postwork回放得到整数1915，与一次联合
成绩相符。报告将这项结果标为 `baseline_established=true`、
`results_captured=true`，同时标为 `all_scripts_completed=false`、
`program_matrix_passed=false`。40个组正常结束不等于其中的测例都通过；
组内失败、跳过和loader错误继续以原串口及结构化观察逐项保存。

固定 `references/oscomp-autotest@d1bb3a3c4b27274e196a2648518525c1a304e339`
的 `kernel/postwork.py::postwork()` 对每种libc的RV/LA原LTP分之和应用
`500 * log10(1 + 9 * clamp(raw, 0, 10000) / 10000)`。本次非LTP合计
1704.1357881746039，LTP-glibc原分920+908=1828，换算211.229257015972，
LTP-musl未运行贡献0，最终1915.3650451905762，Job取整数1915；不得直接相加44格。

两侧原 `fs_fill` 都在测试设备准备阶段报告 `Failed to create test_dev.img: ENOSYS`
和 `Failed to acquire device`，TBROK、shell status 6。两侧没有新的
`BoarOS: fatal`。总预算停止后PID1、页、堆、任务栈、根盘及设备owner未验证。
LA已运行组还报告一些原 ELF loader错误；这些与 `fs_fill` 准备错误和总预算
分别归类，不据 `Accpted` 字样合并成通过。

完整原结果在核对期间位于忽略的 `build/oscomp-official-final/`；采集器重算使用
`python3 -B tests/oscomp/official.py --collect-existing build/oscomp-official-final`，
只重读同一运行且不另启QEMU。按清理规则移除的原始日志不是长期档案。本轮还并行
运行主机正确性回归；1915为原Harness的当次计分，不作为无干扰吞吐或时延对比。
预算截止监督适配基线已经建立，44组完整执行的程序矩阵没有完成。

## LA零分组的后续定位

原LA盘SHA-256为 `1aa79d03cf41e2a80ae4ed43771101c1e67ec8db41c3c20b77792fe6b1b85b50`。
从该盘读取的musl原ELF均为LA LP64D、16KiB LOAD对齐；cyclictest、iozone、
netperf/netserver和entry-dynamic.exe的PT_INTERP是
`/lib64/ld-musl-loongarch-lp64d.so.1`，而原bootstrap只发布标准`/lib`别名。
这一遗漏属于oscomp-compat，不是内核架构缺失，也不是需要替换原libc的理由。

原cyclictest SHA-256为
`21c81ebe791caa060a72fcacf1ed4e2b24db289f9af0e872c2fd91ba5c311d38`，
原iozone为 `21f518549d21cefce221826c91b7fbc8bdda8583a84682de5000e7c4d0a01c26`，
原netperf为 `0a10da9793cc7462169f7ef3eedf7c4ceb7d5591d27570724470622d7f697f2f`，
原libc.so为 `816cff1d1abbef3f1423bbce01a56f00c97b6ff8e20d49966c9d4b6d27e5e7be`。
相同程序/参数在固定Linux
`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`及BoarOS，
512MiB/1GiB均复现缺解释器时exec errno2；只补原盘libc链接后cyclictest/iozone
help和netperf version退出0，netserver help退出1，与Linux实际native状态一致。
八次启动均正常收口，不能把help验证当作原性能组或新正式总分。

另一个独立原因是原musl BusyBox对无shebang文本脚本的处理：原
`run-static.sh`/`run-dynamic.sh`没有`#!`，同格式的单条原runtest调用在Linux与
BoarOS都被原BusyBox拒绝并返回`Exec format error`。kernel的ENOEXEC正确。
用户确认后新增libctest-hook.sh，在临时外层副本中仅将两条执行行改为原BusyBox
sh显式解释，原内层内容与judge不变。45项host门禁通过；同一原runtest的静态/
动态argv及显式shell入口在Linux/BoarOS、512MiB/1GiB均打印Pass，native wait
均为256（包装器退出1），无装载错误。BoarOS两种RAM均正常退出、根owner收口、
heap-live=0。完整libc-test列表的分数和逐案例失败仍须原盘诊断，不将这些入口
验证算作整组通过。本地及新容器report公开script_adaptations与hook SHA-256。

补loader后原cyclictest-musl仍在调度查询阶段退出1：原参数
`-a -i 1000 -t1 -p99 -D 1s -q`在固定Linux/BoarOS均报告
`unable to get scheduler parameters`，没有exec错误。上述原libc.so反汇编的
sched_getparam（0x544e0）与sched_getscheduler（0x54500）直接生成-38并调用
__syscall_ret，未发出syscall。固定`references/musl/musl-1.2.5.tar.gz`
（SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`）
的src/sched同名源文件也直接返回ENOSYS；这只能核对行为，不能证明原盘libc版本。
独立整数raw syscall 120/121在两系统、两种RAM均返回0，priority/policy均为0。
因此该失败归属原盘运行时限制，不通过修改内核、原ELF或libc来获取分数。
补loader后的本地原cyclictest组自然结束，glibc原judge分7.445894033199223、
musl0且根资源正常回收；这是派生QEMU的诊断结果，不改写容器1915历史分数。

## 启动修复后的原组诊断

源码固定为`8a1c9d1399e8342b262f00dd1cfe985bf8cb7e4e`，干净树；原盘身份和
judge仍取上述固定Harness。每个选组由一次本地启动完成，1GiB/单CPU、无RNG。
libc-test一次调用同时启动RV/LA，iozone和netperf是各自的LA诊断；不得把这些
分数相加为正式容器总分。RV使用宿主QEMU11.1.2，LA使用派生RTC QEMU11.1.0，
与官方QEMU10.0.2和容器GCC13的成绩分开。部分诊断与Linux对照并行，性能数字
只用于原judge运行结果，不作为受控吞吐对比。

| 原组 | RV glibc / musl | LA glibc / musl | 行为与退出 |
|---|---|---|---|
| libc-test | 178 / 217 | 179 / 217 | 两种libc全列表结束，musl的217条静态/动态调用均打印Pass；glibc仍有失败。 |
| iozone | 未选 | 30.63088510217055 / 30.05100864020287 | 原两脚本结束，无loader错误。 |
| netperf | 未选 | 8.879897986886947 / 8.80895080473128 | 原两脚本结束，保留so_dontroute、enable_enobufs的errno92及getprotobyname诊断，不能以有分数推断可选接口均支持。 |

上述BoarOS启动均无fatal，PID1退出0，页/堆/任务栈/根盘及设备owner正常收口。
LA-musl libc-test、iozone、netperf的历史零分已经跨过启动阻塞；cyclictest-musl
仍属于上一节证明的原运行时ENOSYS，不能冒充修复后的成功案例。

同一原libc-test列表另在固定Linux、1GiB运行，使用同一bootstrap内容与原
BusyBox，Linux/init helper取`tests/loongarch/root_linux_init.c`，编译定义
ROOT_CREATE_SESSION，RV另用ROOT_DIRECT_FILESYSTEM。脚本由bootstrap显式
解释，Linux先启用lo、退出后撤销proc和/dev/shm临时挂载；原程序及列表不改。
最初Linux未启lo时两种libc的两个socket调用均由原runtest超时，属对照环境缺失，
纠正后再跑完整列表，没有拼接结果。LA的434条原调用与Linux逐ID、形态及
Pass/FAIL状态完全相同：glibc179成功/38失败，musl217成功。38项glibc共同失败
包含原runtest的setvbuf_unget超时，不是LTP监督超时；不修改原程序或libc获取通过。

RV Linux同样为179/217；无RNG的BoarOS多一条静态clock_gettime失败，原断言
同时要求errno为0，实际为EAGAIN。固定源`references/oscomp-testsuits`
commit`8b58dd16d26d30f7c74d48d5832d870d3051b703`的
`libc-test/src/functional/clock_gettime.c`没有在调用前清零errno。额外真实RNG
诊断中可信设备提供64字节，原ELF该项通过，RV恢复179/217且正常回收。这证明
差异依赖熵环境；后续已逐指令定位首次errno写入，见下节。不把额外RNG带入正式
配置，也不让内核伪造随机ready。逐ID失败状态见
[原glibc失败对照](oscomp-libctest-original-failures.tsv)；该表不包含日志或偶然PID。

可重建本地组与环境诊断：

```sh
make test-oscomp-host
python3 -B tests/oscomp/run.py --arch both --groups libctest --diagnostic-timeout 600
python3 -B tests/oscomp/run.py --arch loongarch --groups iozone --diagnostic-timeout 240
python3 -B tests/oscomp/run.py --arch loongarch --groups netperf --diagnostic-timeout 240
python3 -B tests/oscomp/run.py --arch riscv --groups libctest --rng --diagnostic-timeout 180
make all
```

新的官方容器整次运行尚未执行，1915仍只指原`11e96a9`冻结运行；原LTP监督的
300秒/TERM后2秒KILL及34项源码helper表未改动，LTP-musl总预算未到达也未改记通过。

## RV静态clock_gettime的errno来源

对原盘`/glibc/entry-static.exe`，SHA-256
`f140123cee82e5a0a1fc48ef9d845f24be920dfac9a07e5bd31ead053d3657ef`，
使用QEMU插件观测指令、syscall返回和TLS写入，程序字节不变。结果确定为原静态
glibc的malloc初始化污染errno，clock_gettime本身成功；原测例确实捕获了进入
main前errno非零的运行时问题，不能用“成功后的errno检查偏强”代替这条具体归因。

实际指令链：malloc初始化`0x518c6`入口errno=0，调用
`getrandom(0x10c668,8,GRND_NONBLOCK)`，非阻塞随机接口未ready时返回-11；
原glibc在`0x7e85a`执行`sw a4,0(a5)`，把11写到`tp+136`的errno。初始化
随后通过两次CLOCK_MONOTONIC查询生成回退随机位，两次查询均成功但未清除此errno。
进入原main`0x23692`时errno=11；原测例调用CLOCK_REALTIME后在`0x10e32`
返回0，errno仍为11，断言因此失败，native wait=256。该errno字段位置另由原
`__errno_location`代码及TLS store交叉核对；观测地址只用于这个已校验SHA的外部ELF，
不是内核中的测试特判或通用glibc布局假设。

`make diagnose-oscomp-clock-errno`在512MiB/1GiB各验证四种profile：

| profile | getrandom可见返回 | main入口errno | clock返回 / errno | 原child状态 |
|---|---:|---:|---|---|
| BoarOS，缺RNG | -11 | 11 | 0 / 11 | 256（退出1） |
| BoarOS，真实RNG完成后 | 8 | 0 | 0 / 0 | 0 |
| 固定Linux，正常输入 | 8 | 0 | 0 / 0 | 0 |
| 固定Linux，显式返回故障注入 | -11 | 11 | 0 / 11 | 256（退出1） |

最后一行仅把原glibc可见的getrandom返回值注入-EAGAIN，Linux实际返回8；
它证明同一原程序在相同错误返回下也会污染errno，不冒充Linux实际未ready启动。
Linux时钟可能走vDSO，插件仍在原测例的C调用返回点记录0。八次root均正常收口；
host门禁拒绝缺少首次errno写入、缺少main、未知clock错误或native wait缺失的归因。
工具不参与正式runner、judge或原测例清单，使用前需QEMU plugin API及glib-2.0
开发文件，运行后`make all`恢复双架构评测默认配置。

固定Linux仍为`references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`；
其`drivers/char/random.c::getrandom`在未ready且GRND_NONBLOCK时同样返回EAGAIN。
本地固定`references/glibc/glibc-2.44.tar.xz`的NEWS明确列出BZ29624：malloc使
进入main时errno不为零；malloc现使用不会污染errno的内部随机接口。
原RV盘共享libc自报Ubuntu GLIBC2.35-0ubuntu3；静态ELF归因以它自己的字节和
指令观测为准，不用共享库版本字符串推导静态链接身份。
固定archive缺少当年修复历史，因此2026-10-07补核GNU官方
[BZ29624修复讨论](https://sourceware.org/pipermail/libc-alpha/2022-September/142331.html)
及[后续内部接口修复](https://sourceware.org/pipermail/glibc-cvs/2024q1/083880.html)
（commit`5a85786a9005722be7cb9e70f8874a5f1130daea`）。不修改原盘libc以获得分数；
正常新用户环境使用已修复runtime，正式原盘仍按真实熵输入报告该限制。

LA为何不多失败这项也有直接代码差异：原LA静态ELF SHA-256为
`e31723e58c961424e685aba297fbc6a94ad4dc7c2a5aa84d391261bae87c10e2`，
malloc初始化在`0x1200573b0`直接执行getrandom syscall，随后比较原始返回值是否
为8；错误时直接走时钟回退，没有经过把错误写入errno的公共getrandom包装器。
原LA盘共享glibc自报2.38；这里只用静态ELF自己的代码证明初始化路径差异。
因此原clock计分的1分差异来自原运行时初始化，不是LA时钟接口比RV更完整。

其他已核对的共同失败也有具体触发条件：原glibc的`mbc`无法加载任何请求的UTF-8
locale，停在codeset=ANSI_X3.4-1968；`strtol/wcstol`以无效base37调用并要求
endptr更新，glibc返回EINVAL时保留原endptr；`regex_ere_backref`要求将ERE中的
反向引用当成字面数字，而glibc支持该扩展；`strftime`的带符号年份及宽度格式输出
与原断言不同。这些结论取上述保留的Linux实际输出与固定
`references/oscomp-testsuits@8b58dd16d26d30f7c74d48d5832d870d3051b703`的
对应functional/regression源码。它们不能归并为clock/getrandom失败；剩余取消、
stdio等项仍须分别调查实际断言和路径。

## basic的90分与原musl调度接口差异

2026-10-07在干净`8ce615cf1c3f2104ee14b23345bbee71c70b0843`上重新跑原basic：
单CPU、1GiB、无RNG，每侧一次启动，同时执行glibc/musl两个原脚本。RV/LA四份
原judge结果完全相同，都是90/102；两侧PID1退出0，根owner释放、heap-live=0。
原judge的32条记录中只有以下三条扣分，不能把90解释为90/100或装载失败：

| 原judge项 | pass/all | 丢分 | 首个可证实原因 |
|---|---:|---:|---|
| test_brk | 1/3 | 2 | 原ELF自己的raw syscall包装器把64位返回地址截断为32位 |
| test_mount | 0/5 | 5 | 请求vfat，内核尚未实现，mount返回-ENODEV |
| test_umount | 0/5 | 5 | 前置vfat mount同样失败，未到达实际umount |
| 其余29条 | 89/89 | 0 | 原断言通过 |

判分依据为固定`references/oscomp-autotest@d1bb3a3c4b27274e196a2648518525c1a304e339`
的`kernel/judge/judge_basic-{glibc,musl}.py`。测例源码依据为
`references/oscomp-testsuits@8b58dd16d26d30f7c74d48d5832d870d3051b703`的
`basic/user/src/oscomp/{brk,mount,umount}.c`。重建原组结果：

```sh
python3 -B tests/oscomp/run.py --arch both --groups basic --diagnostic-timeout 120
make all
```

原RV `/glibc/basic/brk` SHA-256为
`3756dce8d8734a564300ca4404e6b80136f3d369672c03afaffab63da1fae086`，
包装器在`0x1e0c`执行ecall，`0x1e10`随即执行`sext.w a0,a0`；原LA同路径
ELF SHA-256为`d3882df3c12108f783d23be0db1eb66429f750686d54ac1151f545067f0c9310`，
在`0x2474`执行syscall，`0x2478`执行`slli.w a0,a0,0`。两条指令都会将
低32位符号扩展，丢失真实地址高位。外部只读QEMU指令观测得到：

| 架构 | 内核brk(0)原始返回 | 原包装器返回给调用者 | 后续错误请求 |
|---|---|---|---|
| RV | 0x3564c2e000 | 0x64c2e000 | 0x64c2e040 |
| LA | 0x557d6a210000 | 0x6a210000 | 0x6a210040 |

上述地址只是一次ASLR样本。两侧调用同一`kernel/syscall/memory.c`和
`mm/mm.c::kernel_mm_brk`：错误请求低于start_brk时返回旧break，符合raw brk契约。
高地址PIE由共用`kernel/elf_image.c::choose_pie_bias`按目标用户空间布局选择；
不通过修改raw返回宽度或对原测例安排特殊低地址来掩盖截断。
固定测例源码中的包装器声明与该发布ELF的截断不一致；这里归因于实际发布字节，
不据此猜测它的编译来源。修正原程序包装器才是对应的软件修复，但正式输入保持不变。

mount/umount的实际输出均为`Mounting dev:/dev/vda2 to ./mnt`后`mount return: -19`。
共用`kernel/syscall/mount.c`目前只接入ext4/tmpfs/devpts/proc，vfat在解析设备前
返回ENODEV。原盘也没有`/dev/vda2`节点，正式配置未提供该分区环境；仅建立名字
不能创建真实分区及文件系统。恢复这10分需要真实VFAT能力和对应块设备/分区环境，
属于共同内核与准备依赖的后续工作；不能把umount失败归为已到达卸载路径。

原盘musl的调度函数还有独立的内容差异。RV `/musl/lib/libc.so` SHA-256为
`a174c80743882436816923d3afd8ca4a69ca92a89c88332ac95334052e2698bf`，
自报musl1.2.0、riscv64-sf；LA同路径SHA-256为
`816cff1d1abbef3f1423bbce01a56f00c97b6ff8e20d49966c9d4b6d27e5e7be`，
自报musl1.2.5、loongarch64。实际反汇编如下：

| 函数 | RV原libc | LA原libc |
|---|---|---|
| sched_getparam | 0x4dad8，ecall 121 | 0x544e0，直接__syscall_ret(-ENOSYS) |
| sched_getscheduler | 0x4dafc，ecall 120 | 0x54500，直接__syscall_ret(-ENOSYS) |
| sched_setparam | 0x4db44，ecall 118 | 0x54544，直接__syscall_ret(-ENOSYS) |
| sched_setscheduler | 0x4db68，ecall 119 | 0x54564，直接__syscall_ret(-ENOSYS) |

因此RV原cyclictest-musl会进入内核查询/设置调度并获得样本；LA在首轮查询即退出1，
无样本而得0。两架构内核使用同一`kernel/syscall/sched.c`，此前raw120/121在
固定Linux和BoarOS、512MiB/1GiB均返回0；继续补LA内核分支不能改变未进入内核的函数。

这不是“旧musl支持、新musl移除”。固定本地1.2.5源码直接返回ENOSYS；另外从
[musl官方1.2.0归档](https://musl.libc.org/releases/musl-1.2.0.tar.gz)核对同名四函数
也都是ENOSYS，归档SHA-256为
`c6de7b191139142d3f9a7b5b702c9cae1b5ee6e7f57e582da9328629408fd4e8`，
2026-10-07访问，分析缓存路径`build/tools/oscomp-analysis/musl-1.2.0.tar.gz`。
归档没有RV专用sched_getparam覆盖，原RV二进制实现内容与上游同版本有差异；
版本字符串不能证明具体构建来源，修改者及构建历史未确认。
上游2012-11-11的[调度接口取舍记录](https://git.musl-libc.org/cgit/musl/log/include/pthread.h?h=v1.2.3&showmsg=1)
（commit`1e21e78bf7a5c24c217446d8760be7b7188711c2`，2026-10-07访问）说明，
Linux sched syscall提供线程调度，而POSIX同名接口要求进程调度，musl选择不发布
这一可选能力并返回ENOSYS。这是上游语义选择，不能称为LA架构漏接syscall。

若要支持依赖Linux调度语义的cyclictest，修复归属是提供真实Linux调度接口的
用户运行时，四个函数都应执行实际syscall并正确传播错误；原正式盘仍保持原身份。
两侧原运行时并非功能等价：RV旧glibc的初始化errno问题使clock少1分，原RV
musl的实际调度接口又使cyclictest能运行。分差不能用于推导内核架构能力高低。
本轮只完成原输入差异归因及basic原组回归，没有修改内核、原libc或原judge。

## 公开用户态修正对照

2026-10-07用户确认最小修正与兼容DSO路线。新增
`tests/oscomp/user_runtime_probe.py`和`make diagnose-oscomp-user-runtime`，只执行
独立诊断，不接入正式runner。原发布盘完整SHA在每侧启动前后核对；只允许写入
build内非符号链接临时盘。原cyclictest、libc及原脚本/judge保持字节身份。
修正版basic ELF只发布到临时盘，报告明确标为`repaired-user-runtime-diagnostic`、
`official=false`，记录输入、修正、工具、Linux、内核和每次实际启动的身份。

brk修正严格绑定上一节的原SHA、ELF机器类型及唯一可执行LOAD文件范围，
拒绝指令变化、歧义映射和宽度变化。仅把返回后的截断替换成同宽NOP：

| 架构 | ELF指令地址 | 原字节 → 修正字节（小端） | 修正ELF SHA-256 |
|---|---|---|---|
| RV | 0x1e10 | 0125 → 0100 | 1bb8cfd3f372a85ab63eeaf2044c2000dbfdc9aebcaaa3918d130200de429c2b |
| LA | 0x2478 | 84804000 → 00004003 | 2912c15f3e60560d5ab6e115479da7fedebe440d0a9f7b9b9c7ceda42a479b77 |

每架构原glibc/musl目录中的brk字节相同，适用相同修正。只读QEMU插件同时记录
请求、内核原始返回和用户包装器返回；每次启动覆盖两种libc的修正前后共20次调用。
验收要求地址高于32位范围，修正后实际完成base+64/base+128增长并完整返回地址，
不能只凭原程序显示的低32位十进制输出计为正确。

`tests/oscomp/sched_compat.c`的LA LP64D共享对象通过原musl的公共syscall接口
执行118/119/120/121，并借用原libc的线程errno。原cyclictest的动态JUMP_SLOT
允许LD_PRELOAD接管这四个符号；原ELF及libc无需替换。实际DSO SHA-256为
`823cec886ed56d4dcee17f7177b667a2dfb96547e15d05fc974780940fad0319`。
仅在独立sched probe和LA musl cyclictest子进程中开启preload，basic与原对照
显式关闭。probe核对查询/设置、成功时保留errno以及EFAULT/ESRCH/EINVAL；
不加DSO时退出91，加DSO时退出0。原cyclictest单线程参数在两系统、两种RAM下
不加DSO均native wait=256，加DSO均wait=0且有真实采样。

验收基于`fb6889d`后的本改动、固定Linux commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`，单CPU、无RNG；RV/LA各在Linux与
BoarOS的512MiB/1GiB执行，八次启动均正常结束。BoarOS的页/任务/设备根owner
校验通过、heap-live=0；Linux诊断PID1在应用退出后wait收养的后代，再卸载根盘。
该ROOT_REAP_CHILDREN选项只在本诊断启用：原压力脚本发送SIGINT后并不wait
hackbench，脚本结束不能证明400个后台worker已退出。八次basic原judge均为
90/102→92/102，brk为1/3→3/3，VFAT相关10分仍缺。

完整原LA musl cyclictest脚本四场景均报告子程序成功，BoarOS两种RAM的18条
线程记录均有采样；原judge的诊断分如下。Linux压力八线程的零采样仍公开记录，
不能以四个success或原judge分数替代每个线程的进展证据。

| 系统 / RAM | 原judge诊断分 | STRESS_P8零采样线程数 |
|---|---:|---:|
| BoarOS / 512MiB | 7.365689562094878 | 0 |
| BoarOS / 1GiB | 7.3843744828409665 | 0 |
| Linux / 512MiB | 7.438810171799261 | 3 |
| Linux / 1GiB | 7.540831489481921 | 1 |

这些启动包含只读插件，部分与其他诊断同时运行，不作为独占吞吐/时延比较，
也不替换1915历史正式总分。另一次Linux basic观察到父子`cpid`字符输出交错为
`cpid: 222cpid: 0`，原judge要求独立行而扣pipe分；报告保留其他basic分差，
不把两次独立执行的差异自动归因于brk修正，不修改原输出或judge。

```sh
make test-oscomp-host
make diagnose-oscomp-user-runtime
# 也可单独选择架构；入口结束或失败后自动恢复默认双架构make all。
python3 -B tests/oscomp/user_runtime_probe.py --arch loongarch
```

host反例覆盖原身份/指令拒绝、真实高地址返回、缺失/重复native wait、子程序
失败与零采样分类，以及禁止写入参考盘。保护反例本身使用临时输入；本轮一次
误用真实LA原盘路径后，已从固定xz重新恢复并完整核对原SHA，恢复后的验收
再次核对两侧原盘启动前后身份。结果收口后按既有pruner清理运行副本与日志。
