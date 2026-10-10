# 将本机验收环境迁入主线 CI

2026-10-08 的调整针对主线已经交付的单核 RV/LA 能力。旧 CI 的两个 job
实际只有 RV；增加 `make test-loongarch` 也只覆盖其首阶段聚合依赖，不能替代
后来独立交付的 SIMD、动态 TLS、PCI、终端和设备生命周期门禁。
当前执行组合和入口见[CI 模块](../modules/continuous-integration.md)。

## 同版本仍不足以复现 GNU 验收

原 GNU profile 绑定本机绝对路径和文件身份：RV glibc 2.44、LA glibc 2.42。
Ubuntu 安装同名交叉编译器不会自动满足这些二进制、CRT、headers 或安装树。
实际查得 LA Loongson 2025.08.08 发布包与既有 GCC 15.1.0 和 glibc manifest
全部身份一致；RV 原安装来自 Arch GCC 16.2.1-1、binutils 2.47-1、glibc 2.44-1
和 Linux API headers 7.2-1。原包恢复后再次核对固定 GNU manifest，保留原字节。

LA 发布依据为
[Loongson release 2025.08.08](https://github.com/loongson/build-tools/releases/tag/2025.08.08)，
其资产 API SHA-256 为 `b8572e2083143ff1807658f02e11eba53e5ed81d6194854d369b43fceea72de7`。
RV 包取自 [Arch Linux package archive](https://archive.archlinux.org/packages/r/)，
逐包 URL/SHA-256 归 `references/sources.tsv`，访问日期均为 2026-10-08。
安装位置迁移后仍检查 release 字符串、工具/runtime 哈希、内容、权限和链接。

RV 包的 GNU headers 位于 target 的 `usr/include`，Linux headers 另位于 target
的 `include`；原 libc 链接脚本使用 `/lib` 与 `/usr/lib`，因此 sysroot 必须是
`<package>/usr/riscv64-linux-gnu`，不能误用整个包目录。迁移后的两侧五形态
GNU 程序已实际通过固定 Linux/BoarOS，LA 含两种 RAM。Ubuntu 24.04 容器能
运行两侧原编译器并编译原 RV GNU probe；这些本地结果和首次托管结果分列。

## 缓存不能重新确认自身损坏

旧 LA 准备器在命中缓存后执行 make，再把当前 vmlinux 的哈希写回身份文件。
如果已损坏的 ELF 时间较新，make 不会重链接，却会给它登记新身份。host 反例
分别复现损坏和缺失 ELF：现在在调用 make 以前核对既有 image_sha256，失败
明确拒绝。固定依据仍为 `references/linux@f4cdf7ca9a1fdcca413157df19753f388a5a224e`
与 `references/qemu@84f07211cc5b4fc6a371559bf8a5de4fb068e648`。

共用 CI runner 保留命令的完整输出并分别记录结果；一项失败继续下一独立组，
不是把失败覆盖成最后一项成功。timeout、缺工具、缺失 job 和 skipped 都不能
通过汇总；取消时未执行项保持 incomplete。原程序清单必须显式严格判定。

整体审查另确认外层缓存必须包含 BusyBox 输入清单、RV Linux builder/config
及其宿主工具身份；否则正常更新会恢复旧 key 并被内部身份拒绝，或新内层
产物无法重新保存。已用两种架构的输入变化反例保护新 key。producer 本身
无论成功失败都上传准备日志/身份；consumer 按实际 network、TTY/PTY 和
原程序运行目录归档完整输出，失败镜像单列，不能只依赖聚合日志中的尾部。

## 首次干净 runner 暴露的工具边界

Ubuntu 24.04 的 GCC 13 不支持 `-fno-link-libatomic`。原 Makefile 会先检测，
网络和终端 Python runner 却无条件添加，因而在用户程序编译前失败。共同
架构 profile 现在按实际 musl wrapper 检测；LA 仍保留整数 ABI 和 16 KiB
ELF 对齐。Ubuntu GCC 13 已真实编译 network、TTY/PTY 和环境的六个探针，
不能用本机 GCC 16 的成功代替这项验证。环境 shell 还需要显式安装 ripgrep，
TTY/PTY 所需原完整 BusyBox 也必须在干净 RV job 中先构建，不能依赖本机产物。

原程序 driver 的 exec 错误管道原先将 `write` 返回值直接丢弃，Ubuntu Fortify
使其成为 `-Werror` 编译失败。现在处理 EINTR 并检查实际写入，始终保留原
exec/setup errno；host 编译开启 Fortify，另有一次中断后的真实错误回传探针。
独立 inventory 入口还因架构 import 把 `tests/` 放到本目录前面，误导入另一个
`environment.py`。保留脚本目录优先级后，29 项 inventory host 测试在本机和
Ubuntu 均通过；严格原程序执行仍单独判定，入口修复不等于程序通过。

首次 LA 冷环境准备超过 45 分钟；它包含 QEMU、两个 Linux 配置与整数 GCC/musl
的源码构建。main 新 push 不再取消正在构建的环境，最新 pending 执行随后使用
精确缓存；PR 继续取消旧执行。并发依据为
[GitHub workflow concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
（2026-10-08 访问）；这是缓存生产生命周期的调整，未放宽 job 的通过条件或预算。

## 共同层级与已证实的内核等待错误

[运行37964071894](https://github.com/Freefor100/BoarOS/actions/runs/37964071894)
对应旧身份`16951f0`，两个environment、shared host、RV ABI、两侧GNU/runtime及
LA core/platform均成功。失败项是RV可睡眠存储，required按真实失败拒绝。
此前输入恢复与网络准备修复已在托管执行通过，不能把新失败再归为同一环境错误。
设备已发布完成但batch在IRQ-off跳过park而未收割的问题见
[存储等待记录](sleepable-storage.md#dma发布早于等待交接)。

旧RV工作流还在首个step失败后跳过后续双盘/userland/platform/栈检查；LA使用
runner分组后继续，SQLite又位于不同job，造成报告与覆盖层级不一致。现在两侧
core/ABI、runtime/SQLite、platform共用执行模板，每个架构保持独立producer。
共同消费者显式执行512MiB/1GiB；RV SQLite DELETE改用现有共同runner，Linux
对照、静态/动态CLI及独立重启与LA一致。原单架构聚焦入口默认值保持。

CLI反例先证明缺RV core及RV runtime缺SQLite，再保护两侧注册与显式RAM参数。
共用runner原有失败后继续、取消/缺失和完整尾部门禁保持；新模板的platform以
实际native准备结果为前置，不借core成功决定是否执行。架构扩展据实际设备和
已验收消费者保留，不能为了job表面对称创建成功占位或删掉RV已有门禁。

本轮本地执行新清单的两侧core/runtime/platform，各为8/3/3组，共28组全部通过，
没有跳过或补记；审查补齐后的24项CI host、3项RAM profile及actionlint1.7.7也通过。两侧
1366 ABI、GNU五形态、SQLite DELETE/WAL/独立重启和平台消费者实际执行双RAM；
这些本地结果与修复后托管运行仍分别记录。

独立审查发现模板迁移漏归档共同ABI目录及漏掉reference恢复门禁，已集中补齐。
新增反例先拒绝缺失原串口/metadata/normalized/diff/失败盘和test-references的配置，
修复后通过；patch-format仍保留，host checkout取得实际父提交。

## 宿主动态依赖与实际编译器选择

[运行37978208815](https://github.com/Freefor100/BoarOS/actions/runs/37978208815)
在`a74c219`上确认此前block等待修复通过，双方环境准备、host、LA两层及RV原生C
也通过；RV core和runtime分别因ELF-tail与SQLite DELETE的宿主准备错误失败。
ELF-tail尚未启动客体：PATH中的固定Arch readelf缺`libdebuginfod.so.1`而退出127。
SQLite已生成临时盘，但raw supervisor编译器写死为本机的`riscv64-elf-gcc`，CI只有
`riscv64-unknown-elf-gcc`。两项均不能归为内核或原SQLite程序失败。

文件身份相同不保证宿主共享库可用。保留原RV GCC16.2.1/binutils2.47与LA
GCC15.1.0/binutils2.45包身份，Ubuntu另安装`libdebuginfod1`；准备器实际运行六类
GNU工具的`--version`，在导出PATH及保存缓存前拒绝启动失败。Make的`CC`传入
SQLite共用runner，而不是在CI创建本机工具名称的别名；实际compiler身份随结果保存。

验证必须使用独立Ubuntu PATH、空宿主LD_LIBRARY_PATH和恢复出的原工具包，
不能把本机同名程序、已安装共享库或既有ELF的成功作为CI环境证明。host反例保护
工具启动失败和Make→wrapper→runner的选择传递；真实验证入口仍为
`make INIT_CONFIG=config/init.json TEST_MEMORIES='512M 1G' test-elf-tail-riscv test-sqlite-rollback-riscv`。

2026-10-10修复验收：Ubuntu24.04的独立PATH下，实际恢复工具启动、27项host测试和
同ELF Linux/BoarOS ELF-tail通过；SQLite双RAM的DELETE、静态/动态CLI及独立重启
12阶段通过，raw compiler为Ubuntu GCC13.2的`/usr/bin/riscv64-unknown-elf-gcc`。
仅Git文件、无参考archive或build缓存的Ubuntu checkout也通过全部27项host测试。
LA共用SQLite runner的双RAM12阶段及六类固定工具启动检查通过。上述是本地隔离环境
证据，后续托管结果单独核对。
