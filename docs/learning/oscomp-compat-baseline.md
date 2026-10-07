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
差异依赖熵环境，尚未逐指令归因errno的首次写入；不把额外RNG带入正式配置，
也不让内核伪造随机ready。逐ID失败状态见
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
