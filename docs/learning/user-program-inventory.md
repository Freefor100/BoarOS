# 固定真实程序的失败清单

入口收集外部程序的构建、运行与语义失败。清单生成成功不等于程序通过，也不是比赛成绩。
环境缺失必须先修复；不能把未构建、未运行的程序作为 BoarOS 不兼容的证据。

## 输入与构建

`references/oscomp-testsuits` 是完整 Git 对象库。默认固定的决赛 commit
`b5ec6ef8497e1818cbdec3b54bb722f036e57972` 不含 libc-test，仅能说明这个 commit 的目录内容。
它不能证明上游没有可用 libc-test。程序配置现在额外固定 `pre-2025` 分支选定时的 commit
`8b58dd16d26d30f7c74d48d5832d870d3051b703`，通过 `git archive` 提取 libc-test 与原始脚本，
不切换参考工作树 HEAD，也不追踪浮动分支。

`tests/program-inventory/inputs.json` 明确区分 BusyBox 源码、比赛配置、初赛脚本和 libc-test
来源。BusyBox 仍用决赛 commit 的原 `config/busybox-config-riscv64`，398 个 applet 全部保留。
旧版本的 `tc` 依赖已被当前 Linux UAPI 移除的 CBQ 定义，因此使用完整、校验过的 Linux v6.6
UAPI 编译。运行内核仍是 `references/linux` 的固定 commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e`。缺 `linux/kd.h` 是原工具链环境没有导出 Linux
UAPI，补齐正确的目标头即可解决；不是 BoarOS 的能力结论。详细来源、工具身份与缓存约束见
[用户程序环境模块](../modules/program-environment.md)。不再提供裁剪配置的 smoke 回退。

libc-test 使用已有 musl 1.2.5，运行原始 `make disk` 配方；只在命令行指定交叉编译器、
工具链兼容选项和客体解释器路径 `/lib/ld-musl-riscv64.so.1`，不修改上游 C 源码。
构建原始静态/动态 entry、runtest 和 DSO，并将 musl loader、libc.so 和依赖 DSO 放入镜像。
静态表有 107 项、动态表有 110 项，每项用上游生成的 dispatch 名称直接执行。
另外执行两份原始 libc shell 包装脚本，独立暴露 runtest 自身的系统调用依赖。

BusyBox 采用 GPL-2.0-only，libc-test 的许可见固定输入 `libc-test/COPYRIGHT`。
源码、配置、许可证和构建产物保存在忽略的 `build/` 中，不纳入本仓库。

## 复现与判定

```sh
make inventory-userland-riscv
make test-program-inventory-host test-diff-abi-host
# 明确复用已校验的构建产物，所有程序失败也要求非零退出：
python3 tests/program-inventory/run.py --reuse-builds --require-pass --output build/program-check
```

默认输出 `build/program-inventory-full/`。首次构建准备完整 UAPI 与程序，不要求人工复制头文件。
`--case` 可选择单例，`--suite` 可选 busybox/libc/all。输入未变时可恢复未完成运行；身份变化必须用
新输出目录，保留之前证据。`--build-only` 只构建，不能当成运行证据。

每个用例分别启动固定 RISC-V Linux 和生产 BoarOS：QEMU system 模式、单 hart、512 MiB、
独立的同源 ext4 镜像副本、同一测试 ELF。用例之间也不共享改动后的磁盘。Linux 使用独立的
`tests/program-inventory/linux.config`，启用真实程序所需的设备、网络、IPC 等能力；不会改变
文件 ABI 差分的最小配置。驱动准备 proc/sysfs、共享内存和消息队列挂载及 loopback；Linux
环境初始化失败单独记录，不得成为有效差分参考。

fixture 包含账号文件、设备节点、临时目录、所有 applet 链接和动态库。BoarOS 不支持挂载、
网络、symlink 或某设备的错误保留在日志中，不以成功存根替代。程序分别记录 stdout、stderr、
wait status、signal、exec/setup errno、超时和客体退出状态；任意输出按十六进制编码传输。
原始日志、独立 stdout/stderr、镜像、构建配置、命令及产物 SHA-256 均保留。

原 BusyBox 脚本执行 55 条命令；检查组边界和完整、有序的每条 success/fail 记录，不能只看 shell
退出码。原 libc runtest 即使打印 `Pass!` 也固定返回 1，故包装脚本预期退出 1，但必须同时具有
每个案例的 START/Pass!/END，不能出现 FAIL、缺项或重复。直接 entry 则预期退出 0。

`runs/suite.json` 和每例 `result.json` 区分 `pass`、`nonzero-exit`、`signal`、`timeout`、
`exec-error`、`setup-error`、`upstream-failure`、`output-mismatch`、客体/协议失败与
`reference-not-pass`。后者表示 Linux 自己未满足契约，必须调查参考环境、测试及 libc，不能
直接归因于 BoarOS。保留双侧状态，单侧错误不隐藏另一侧结果。输出未做宽泛归一化：时间、设备号
等不同可能产生 output-mismatch，需要阅读原始记录区分环境值和语义差异。

默认命令成功表示清单完整生成，程序失败仍列在清单中；构建/执行器异常非零。
`--require-pass` 在任意失败或未完成时也返回非零。没有手工 Linux 参考答案或隐式跳过。

## 证据边界与后续定位

最初六例只测 true、false、echo、cat、ls 和 shell 调用外部 cat，且使用裁剪 BusyBox；其通过
只保护这几个命令的输出与退出状态，不能代表完整 BusyBox，更不能代替 libc-test。现在保留这六个
基础命令，但使用完整比赛配置二进制，并增加原脚本及全部 libc entry。最初完整运行共 226 个顶层案例；
其中 BusyBox 的一个脚本案例内部有 55 条命令，libc 两个脚本案例重复覆盖原 entry 表并增加包装器契约。
不能把 226 当作互不重复的上游测例数。readv 阶段另加完整 BusyBox `od/hexdump` 两个独立输出对照案例，原 226 项均保留。

运行结果和依赖清单见 `docs/goals.md`。对非零退出只记录观测，不凭退出数字推断 syscall 根因；
需要由原始错误输出或最小复现确认。修复顺序按能力依赖和失败簇确定，不按固定测例名称写特判。

环境排查不能只检查磁盘内容：固定 Linux 的 `init/do_mounts.c` 在启动 `/init` 前自动挂载
devtmpfs，镜像原有 `/dev/shm` 会被遮住。首轮 `pthread_cancel_points` 没有打印 shm_open
错误，却在线程 join 后报告错误取消；原因是 shm_open 失败后的测试诊断通过 write 输出，
write 自身成为 pending cancellation 的取消点，原错误输出因而消失。重建可见目录并挂载 tmpfs
后，同一 Linux/ELF 用例通过。类似地，socket 用例访问 127.0.0.1 前必须启用 loopback。
这些都是参考环境准备责任，不能列为内核语义差异。

动态 `argv` 的进一步定位没有修改内核或被测 ELF：GDB 在 `argv_main` 返回点读到
`t_status`（映像偏移 `0x4ddd8`）为 `0x615f696e`，实际退出低八位为 110；该地址起的
`ni_array\0.data.r` 与 ELF 文件同偏移的非装载尾部字节一致，正常 BSS 应为零。
这证明程序已进入 main，不能归因于缺 loader 或把所有动态程序称为不支持。
源码 `kernel/elf64_source.c` 的 run 分界没有将包含文件末尾的局部页从此前完整文件页分开，
已被双侧真实回归证实并修复。调试命令、寄存器、哈希及串口证据保存在
`build/program-dynamic-probe/`；修复前的原始证据继续保留。

## 2026-09-16 补齐环境后的完整运行

执行命令为 `python3 tests/program-inventory/run.py --reuse-builds --suite all --output build/program-inventory-final`。
所有 226 项均完成，Linux 全部满足各自退出与脚本断言契约；BoarOS 双侧一致 104 项、
退出状态不符 119 项、上游脚本断言失败 3 项，无参考环境失败或隐式跳过。

| 集合 | Linux | BoarOS |
|---|---:|---:|
| 完整 BusyBox 二进制的六个基础命令 | 6/6 | 6/6 双侧一致 |
| BusyBox 原脚本内命令 | 55/55 success | 42 success、13 fail |
| libc 静态 entry | 107/107 | 96 双侧一致、11 退出不符 |
| libc 动态 entry | 110/110 | 2 双侧一致、108 退出不符 |
| libc 原静态/动态包装脚本 | 两份所有断言通过 | 两份断言失败 |

包装脚本在 BoarOS 上明确报告 `sigtimedwait: Function not implemented`；不能将其包装器
失败等同于所有直接 entry 都失败。BusyBox 的 13 条失败命令为 df、dmesg、du、which ls、pwd、
free、hwclock、后台 sleep/kill、touch、od、mv、rmdir、find；脚本依赖前序状态，不能把它们
计为 13 个独立内核缺陷。逐项原因仍以 stderr 和聚焦复现为准。

证据目录 `build/program-inventory-final/` 包含 manifest、inventory、运行身份、每例原始日志与
磁盘；`build/program-environment/reproducibility.json` 记录两次独立完整 BusyBox 构建得到相同
ELF、配置及 UAPI 树哈希。BusyBox ELF SHA-256 为
`f2cda5fcdff6d41c8a553ac658e8aa55b6a48aa40898cb123a19f7865f3773ac`。
对同一结果使用 `--require-pass` 已确认返回 1，不会将“清单完成”误报为“全部通过”。

## ELF 文件尾页修复后的动态直接用例

`tests/riscv/elf_tail_main.c` 的真实静态 ELF 使用页对齐的可写装载段、三个完整文件页、
非对齐文件尾和多页 BSS。旧内核在相同 Linux/BoarOS 双侧执行中分别输出
`ELF BSS PASS` 与 `ELF BSS FAIL`；run 边界修复后两侧均输出 PASS。
`make test-elf-tail-riscv` 将该双侧回归接入 CI，保留 QEMU 串口、独立镜像与结果。

同一份固定 libc-test 动态 entry 的 `argv` 修复后双侧退出 0；全部 110 个动态直接
用例在 `build/elf-stage-dynamic/` 重跑，Linux 110/110 满足契约，BoarOS 99/110
双侧一致，余下 11 项与静态失败集合相同。此结果只针对动态直接用例；完整
旧完整运行的 104/226 是 ELF/readv 修复前基线，不应当作当前内核状态。

## ELF 与 readv 修复后的完整复跑

`python3 tests/program-inventory/run.py --reuse-builds --suite all --output build/readv-inventory-verified`
运行了原 226 项及新增的两个完整 BusyBox 命令。Linux 228/228 满足各自契约；BoarOS
205 项与 Linux 的退出状态及完整输出一致，20 项直接 entry 退出不符，3 项原脚本断言失败。
原 226 项中 203 项一致。静态 libc entry 为 97/107，动态为 100/110；两组剩余失败名称完全相同。
固定 libc-test 的静态和动态 `ungetc` 都通过，完整 BusyBox `od -An -tx1` 与
`hexdump -C` 对 `inventory data\n` 的输出也逐字节双侧一致。原 BusyBox 脚本仍是独立案例，
内部 43/55 条 success（此前 42/55），`od` 已恢复，余下 12 条不能据名称一一归因为独立内核缺陷。

直接 libc 的十个剩余名称为 `daemon_failure`、`pthread_cancel_points`、
`pthread_robust_detach`、`rlimit_open_files`、`socket`、`sscanf_long`、`stat`、
`statvfs`、`syscall_sign_extend`、`utime`。原始输出明确报告 `getrlimit/setrlimit`、
robust mutex、socket、statvfs、utimensat 等缺失；`stat` 与 `syscall_sign_extend`
依赖 `/dev/null` 或 `/dev/zero`，并暴露身份调用尚未实现；`pthread_cancel_points`
只报告 shm_open 取消检查失败，原因仍待聚焦复现。两份原 libc 包装脚本依旧打印
`sigtimedwait: Function not implemented`，不改变直接 entry 的 197 项通过事实。
运行身份、逐例 stdout/stderr、原始串口、镜像和比较状态保存在上述证据目录；没有把参考
输出粘入 C 测试，也没有把未运行案例计入通过。

此前一次全量复跑的静态 libc 包装案例输出了完整 `SUITE END`，但 PID 1 退出时报告
`root boot error status=0xb`，因此保留在 `build/readv-inventory-final/` 中并计为
`guest-incomplete`。增加分阶段诊断后，固定内核与 fixture 的 20 次重复中第 11 次复现：
`stage=0x40`，堆已清空，物理页比基线少一页。根因是 boot idle 收到 PID 1 completion
就停止本轮回收；PID 1 的未等待 zombie 子进程此时已被重挂到 exited 队列尾部，
其任务元数据页仍由队列拥有。新增真实 U-mode `/init` 探针让子进程先成为 zombie、
父进程不调用 wait4 就退出；修复前稳定失败，排空该队列后通过。相同包装场景在修复后的
20 次固定内核/fixture 重复中均完成资源清理。诊断重复日志保存在
`build/root-finish-repeat/` 与 `build/root-finish-fixed-repeat/`，原异常运行仍不计入通过数；
这不是 libc syscall 的直接失败。

## 同步信号等待后的原包装脚本

`rt_sigtimedwait` 接入后，用未修改的原静态/动态 libc 脚本分别重跑
`libc.official.static` 和 `libc.official.dynamic`，证据在
`build/signal-wait-wrapper/`。两侧均完成脚本的 107/110 条逐例记录，BoarOS
不再出现 `sigtimedwait: Function not implemented`；两份脚本各有九条上游断言失败：
`daemon_failure`、`pthread_robust_detach`、`rlimit_open_files`、`socket`、
`sscanf_long`、`stat`、`statvfs`、`syscall_sign_extend`、`utime`。
包装脚本内的 `pthread_cancel_points` 此次通过，但先前独立 entry 的失败不能据此
自动关闭；脚本运行顺序和测试上下文不同，需单独复跑。九条失败仍按各自首个 syscall
或设备依赖调查，不能归因于同步信号等待。

## 路径、信号等待与资源限制后的复跑

`python3 tests/program-inventory/run.py --reuse-builds --suite all --output build/next-batch-inventory-final2`
以相同的固定 libc-test/BusyBox 二进制双侧运行 228 项。Linux 228 项满足契约；BoarOS
211 项与 Linux 的退出状态及完整输出一致，14 项独立 entry 退出不符，3 项原脚本断言失败。
对比 `build/readv-inventory-verified/`，静态和动态 `rlimit_open_files`、`sscanf_long`、
`pthread_cancel_points` 共六项转为通过。`rlimit_open_files` 原来报告 `setrlimit/getrlimit`
均为 ENOSYS，现通过；`sscanf_long` 原来报告 `getrlimit(RLIMIT_STACK)` 为 ENOSYS，
现通过。这两组与新增 `prlimit64` 能力直接对应。`pthread_cancel_points` 旧输出是
shm_open 取消状态断言失败；新增实现未改变 shm_open 或取消路径，因此不能把这两项
归因给资源限制。独立四个相关 entry 在 `build/prlimit-focused/` 和
`build/recheck-cancel-sscanf/` 中再次通过，先前异常仍无独立根因。

原 libc 包装脚本仍分别完整运行 107/110 项，其中 100/103 项打印 `Pass!`，
各有七项 `FAIL`：`socket`、`stat`、`utime`、`daemon_failure`、
`pthread_robust_detach`、`statvfs`、`syscall_sign_extend`。脚本自身的固定退出码
不能替代逐条判断。原 BusyBox 脚本最终复跑为 45/55 条 success，余下十条 fail；
比旧清单稳定新增 `du` 和 `find` 两条 success。后台 sleep+kill 在同一内核的两次
清单运行中一过一败，属于未定位的时序敏感项，不能计入稳定恢复。

直接 entry 的 14 项是上述七个 libc 名称的静态和动态版本。原始输出的首个阻塞：
`socket` 缺 socket 族；`stat` 缺 `/dev/null`，且有效 UID/GID 查询仍返回 ENOSYS；
`utime` 缺 `utimensat/futimens`；`daemon_failure` 首先在 musl `daemon()` 的 `chdir("/")`
遇 ENOSYS，其后还依赖 `/dev/null`；`pthread_robust_detach` 缺 robust futex owner-died；
`statvfs` 缺文件系统统计；`syscall_sign_extend` 缺 `/dev/zero`。这些是已定位的能力或
fixture 缺口，不是本批 `prlimit64` 的回归。原始运行清单、双侧串口与逐例镜像保存在
`build/next-batch-inventory-final2/`；此前独立运行保留在
`build/next-batch-inventory/`、`build/next-batch-inventory-final/` 和
`build/next-batch-inventory-verified/`。
上述计数不表示完整 BusyBox 或 libc 兼容。

限额实现的固定依据为 `references/linux/kernel/sys.c` 的 `do_prlimit()`/
`prlimit64`（commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`）：先复制新值、
在目标线程组内取得旧值并提交、最后复制旧值到用户；旧值输出失败不会回滚设置。
同一版本的 `fs/file.c` 在分配新 fd 时检查软限制，`mm/vma.c` 在栈 VMA 扩展时
检查 `RLIMIT_STACK`。BoarOS 以预留栈 VMA + 缺页实现，因而按尚未驻留的栈页
实施软限制；该内部策略与 Linux 的 VMA 扩展点不同，不能仅凭一次溢栈信号断言
所有栈边界一致。`tests/diff-abi/limits.c` 在固定 Linux 与 BoarOS 上记录
open/dup/pipe、跨 PID、fork/exec 和栈故障的可观察结果；初版 128 KiB 栈探针
受 Linux 已扩展 VMA 范围影响不稳定，改为 1 MiB 后连续运行一致。另一个 exec
后调高 STACK 软限制并访问 1 MiB 栈的差分探针最初发现 BoarOS 永久缩小了栈 VMA，
导致 Linux 正常退出而 BoarOS SIGSEGV；保留 8 MiB VMA、在初始装载与缺页分别执行
软限制后，两侧记录一致。
将 exec 子进程的软限制改为非页对齐的 65535 字节又暴露初始预映射地址未对齐、
导致 exec 返回错误；将可提交范围按 4 KiB 页向下取整后，同一用例继续双侧一致。

真实 `test-userland-riscv` 曾在 stop/continue 复合检查偶发返回 74。测试源码中的
子进程在 `SIGCONT` 恢复后立即退出，存在父进程等待 `WCONTINUED` 前已退出的竞争；
这与观察到的失败相符，但当次没有逐项记录 wait 返回，故不能宣称已证实唯一根因。
该测试要验证继续事件，现让子进程持续 yield，直到父进程观察事件并发送 `SIGTERM`，
避免自然退出参与竞争。失败原始日志在
`build/riscv/userland-run.scTdQg/static-userland.log`；修正后多次完整真实 U-mode
回归通过，但这不构成对所有调度时序的证明。
