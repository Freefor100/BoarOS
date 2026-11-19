# OSComp 评测环境适配

`oscomp-compat` 在通用内核之上提供双架构评测启动配置、原程序监督和判分入口。
发布盘、运行库及判分的已知问题见[测例问题](../learning/oscomp-test-issues.md)。

## 原盘和评测框架

两套原盘放在主工作区下被 Git 忽略的 `sdcard/`。下载入口由
[testsuits-for-oskernel](https://github.com/oscomp/testsuits-for-oskernel) 提供。

| 使用阶段 | 对应上游 | 本地解压后的文件 |
|---|---|---|
| 初赛 | [`pre-2025` 分支](https://github.com/oscomp/testsuits-for-oskernel/tree/pre-2025)，[`pre-20250615` 发布](https://github.com/oscomp/testsuits-for-oskernel/releases/tag/pre-20250615) | `sdcard/pre-20250615/sdcard-rv.img`、`sdcard/pre-20250615/sdcard-la.img` |
| 决赛 | [`final-2026` 分支](https://github.com/oscomp/testsuits-for-oskernel/tree/final-2026) | `sdcard/final-2026/sdcard-rv-pub.img`、`sdcard/final-2026/sdcard-la-pub.img` |

把下载的原盘解压一次即可。普通诊断从原盘建立可写运行副本。本地
初赛入口 `make test-oscomp-pre` 以只读方式挂载原盘，并为每个客体创建独立的
QEMU 写时复制镜像（QEMU Copy-On-Write version 2，QCOW2）。虚拟机只写改动块，
本地打包的启动器负责创建写入层，沿用原框架的构建、测试顺序和判分。

将初赛评测框架放在 `references/oscomp-autotest/`，决赛判分仓库检出
`final-2026` 后放在 `references/oscomp-testsuits/`。需要另选盘目录时使用 `--images-dir`。

默认`make all`离线生成kernel-rv/kernel-la/disk.img/disk-la.img。RV/LA分别生成整数raw supervisor、
init.json和头文件；`INIT_CONFIG_RV`/`INIT_CONFIG_LA`独立覆盖，显式共同
`INIT_CONFIG=config/init.json`同时恢复通用fixture。构建中不可让另一组不同配置
覆盖正在冻结的kernel；先构建、复制内核及身份，再运行。

## final-2026 可选入口

`final-2026` 使用公开的 Debian 根文件系统运行 CAgent 和 BuildStorm。每台虚拟机
挂载本架构原盘的独立副本。

```sh
make test-oscomp-final

python3 -B tests/oscomp/final.py --arch both \
  --images-dir /path/to/images \
  --host-vcpu-cpus 8,9,10,11,12,13,14,15 --host-io-cpu 0
```

入口复制原盘，由客体处理盘上的日志恢复。
内核通过 `INIT_CONFIG_RV/LA` 分别构建RV八核、LA八核版本，使用8 GiB内存；
默认单核交付物和旧评测配置不被替换。启动使用盘内`/glibc/busybox`准备proc、sysfs、
devpts、共享内存目录和回环接口，再用盘内bash/sh运行原脚本。
保留原Debian加载器和库，不添加旧盘调度DSO或工具分区。

`--groups cagent`或`--groups buildstorm`可单独选择一题。两架构串行运行，
`--host-vcpu-cpus`须提供八个不同的宿主CPU，并与`--host-io-cpu`搭配使用；
不指定时使用宿主原有亲和性。默认不增加外层时间限制，保留原脚本的超时：CAgent各项
20～35秒，BuildStorm正式编译14400秒。工具链检查、小项目编译和tg-xtask预构建
不计入这14400秒。需要主动截断诊断时，才用`--diagnostic-timeout`设置外层秒数，
0表示不设外层限制；外层中断记为运行不完整，不记为编译失败。

运行输出包括原串口、原判分、脚本状态和输入身份，保存在 `build/`。
工具链检查、编译退出状态和产物检查分别判断。正常退出并通过只读磁盘检查后默认删除
运行副本，`--keep-disks` 可保留。错误或未完成的副本留待调查。

## 官方双盘、启动熵与格式化工具

默认 `make all` 在固定官方容器内完全离线构建两套内核和额外磁盘：RV 使用
`disk.img`，LA 使用 `disk-la.img`。原 Harness 已支持将它们作为第二块磁盘：
平台原流程将 RV 辅助盘复制成 `disk-rv.img`，本地容器直接使用该次构建生成的
`disk.img`。LoongArch 使用 `disk-la.img`。
每块盘均为 2304 MiB 的 MBR 磁盘，固定布局由 `tests/oscomp/inputs.json` 维护。

| 分区 | 起点 / 容量 | 格式与用途 |
| --- | --- | --- |
| `/dev/vdb1` | 1 MiB / 128 MiB | 1 KiB ext4，挂到 `/boaros-tools`，只读工具、脚本、监督器 |
| `/dev/vdb2` | 129 MiB / 64 MiB | FAT32，512 字节簇，`/dev/vda2` 指向此设备 |
| `/dev/vdb3` | 193 MiB / 2048 MiB | 4 KiB ext4，LTP 可重新格式化的 scratch |

e2fsprogs 源归档随仓库保存在 `third_party/sources/`，来源见[第三方组件](../third-party.md)。构建使用官方容器已有的 RV/LA musl 交叉编译器，生成静态
ELF EXEC、LP64D 工具；LA LOAD 段符合 16 KiB 页对齐。宿主 `mke2fs` 和 `e2fsck`
由同一源码构建，FAT32 由该固定容器的 dosfstools 创建。缓存位于
`build/tools/oscomp-local/`，`OSCOMP_LOCAL_CACHE` 可另选路径；检查编译器、libc、
源码、编译参数和实际工具哈希后复用，不构建工具链，也不访问网络。宿主没有这些
固定工具时，本地准备入口用已有 pinned 容器、`--pull=never --network none` 执行同一
准备器；容器使用宿主 UID/GID，继续复用同一缓存。

移植补丁只让 e2fsprogs 的 `is_before_linux_ver` 将 UTS version `#1 BoarOS`
识别为未知 Linux 年代，避免把自身 release 当作旧 Linux 而生成 revision 0。
真实 Linux 继续使用原有版本判断。
`/bin/mke2fs`、`mkfs.ext2`、`mkfs.ext3`、`mkfs.ext4` 均链接真实上游工具，原参数
直接传入，配置使用原版 `mke2fs.conf`。

缓存模板的 LBA 8 起 4096 字节槽始终全零。每次 `make all` 或本地准备交付盘时，
宿主为每个架构分别调用 `getrandom` 取得新的 64 字节，写入带版本、架构、长度和
CRC32 的种子记录。记录只存在于本次交付盘，不能把已启动消耗过种子的盘当作新
启动盘；即使种子消耗后的启动失败，重试也必须重新执行准备，生成新种子。
CRC32 检测意外损坏，种子来源是可信宿主。

PID1 创建根盘 `252:0`、第二整盘 `252:16`、分区 `252:17..19` 及 loop 节点，
先只读挂载工具分区。其 `seed` 程序核对 `/` 的设备身份、整盘与各分区的容量、
512 字节扇区、可写状态、MBR 布局和种子记录；整槽覆零并对原始块设备 `fsync`
成功后才用 `RNDADDENTROPY` 注入 64 字节、声明 256 bit，检查随机源就绪并清除
内存副本。任何失败记录操作和 errno，停止启动。随后创建 `/dev/vda2` 别名；
已有同名节点视为冲突。设置 `LTP_DEV=/dev/vdb3`、`LTP_DEV_FS_TYPE=ext4` 和
`LTP_SINGLE_FS_TYPE=ext4` 后再执行测例。

`make run-riscv`、`make debug-riscv`、`make run-loongarch` 和 `tests/oscomp/run.py`
共用这套准备逻辑。根盘只进行原样复制，工具全部放在第二盘 p1，不通过宿主
`debugfs` 修改根盘。运行副本以 `cp --reflink=never --sparse=never` 复制并 `fsync`，
宿主分配和写盘发生在 guest 计时前。原比赛镜像、LTP/libc 字节及 judge 保持原样。
本地示例：

```sh
make all
python3 -B tests/oscomp/local_disks.py --arch riscv --output build/oscomp-manual-rv
qemu-system-riscv64 -machine virt -bios default -kernel kernel-rv -m 1G -smp 1 -nographic -no-reboot \
  -drive file=build/oscomp-manual-rv/root.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
  -drive file=build/oscomp-manual-rv/scratch.img,if=none,format=raw,id=x1 \
  -device virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1
```

LA 使用 `--arch loongarch` 和独立目录，QEMU 使用 `-cpu la464 -kernel kernel-la`、
移除 `-bios default`，两块盘均为 `virtio-blk-pci`。运行目录必须新建；保留原盘
和模板不直接作为可写后端。`make prune-build` 保留工具缓存。

## 原盘bootstrap与监督

PID1直接exec原盘`/musl/busybox`，bootstrap作为sh -c参数编入内核，监督器、脚本和
LA 调度共享对象从只读工具分区加载。启动创建/bin、设备、proc和/dev/shm，并从目标profile建立
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
的两种libc，本地及新官方report以script_adaptations记录该适配是否启用及脚本内容哈希。

LTP 的临时脚本副本在原单项执行点调用监督程序，测例的参数、内部循环和输出保持原样。
监督程序持有直接子进程和测试进程组；握手放行后测试可调用 setsid，组信号仍作用于测试组。
每个执行项都打印单调时钟 START、END、退出状态和 elapsed_ms，耗时包含启动、等待及收尾。
默认 120 秒发 TERM，再等 2 秒后按需要 KILL 并回收，超时返回 124；准备错误返回 125
并报告操作阶段。直接子进程结束后清理组内后代，脱离组的 daemon 由 PID1 退出路径处理。

共享 `ltp-skips.tsv` 过滤缺少控制器／输入／配套端点的辅助程序，以及按评测预算跳过的 `fs_fill`，
输出原因并返回 125；逐项依据见[评测问题](../learning/oscomp-test-issues.md)。显式诊断排除
使用 EXCLUDE 记录。原 LA BusyBox 的 hush 脚本以显式返回值处理失败和循环结束，兼容其
实际支持的 shell 功能。结果分析按原 judge 分数、LTP 断言、退出状态和计时记录说明表现。

## 调度适配

LoongArch 的发布版 musl 有四个调度函数直接返回 `ENOSYS`。工具分区提供
`linux-sched.so` 动态共享对象（Dynamic Shared Object，DSO），通过同一 libc 的
`syscall` 执行真实调度系统调用。构建使用 LoongArch 64 位双精度浮点调用约定
（LP64D），装载段按 16 KiB 对齐。启动脚本为 LoongArch 的动态 musl 程序统一设置
`LD_PRELOAD`，glibc 进程不载入该对象。

旧评测入口在各 libc 的 IOzone 组内使用带实时优先级 1 的轮转调度
（Round Robin，`SCHED_RR`），由原 BusyBox 的 `chrt -r 1` 启动。调度策略由
测试子进程继承。轮转时间片是系统全局参数，脚本在组开始前将其设为 50 ms，
结束时恢复先前值。Unix 进程标识符 1（Process ID 1，PID 1）维持自己的策略。
这项设置随该启动配置编入内核，由启动后的用户态脚本执行。

## 运行与结果判读

```sh
make all
make test-oscomp-host
make test-oscomp-compat
make test-oscomp-pre
make test-oscomp-final
```

本地诊断允许选择架构和组，例如
`python3 -B tests/oscomp/run.py --arch loongarch --groups ltp --output build/oscomp-la-ltp-check`。
本地官方容器入口从 `HEAD` 打包已提交的代码，工作区修改留在原处。原框架配置对每个虚拟机采用单核、
1 GiB 和 3,600 秒总时限。逐例监督在虚拟机内运行，组切换不会重置整机时限。

报告同时保留原判分、分组状态、测例退出状态和虚拟机停止原因。原判分由同一次
双架构调用生成。组外层正常退出只说明脚本走完了该组，测试断言、准备失败、
监督超时和未到达项各有记录。脚本打印 `COMPLETE` 也只代表遍历结束，虚拟机退出
和资源归还要依据实际关机及设备收尾记录判断。无法观察到停止原因时标为未知。

日志、镜像与结果表都在被忽略的 `build/` 下。采集器发生改动后，可以运行
`python3 -B tests/oscomp/pre.py --collect-existing build/某次运行`，从同一次运行
的固定输入身份、原任务输出和生命周期记录重新生成报告。
