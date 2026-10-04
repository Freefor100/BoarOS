# 用户程序构建环境

`tests/program-inventory/environment.py` 为完整上游程序准备 RISC-V Linux
UAPI 与现有 musl 1.2.5 工具链。它不修改 BusyBox 源码或关闭 applet，
也不把宿主 glibc 头加入交叉编译器搜索路径。

## 入口与固定来源

- `prepare()` 导出 `references/linux` 固定 commit
  `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 的 `ARCH=riscv headers_install`。
  默认目录为 `build/program-environment/linux-uapi/include`，供 libc-test 等程序使用。
- `prepare(uapi='busybox')` 通过 `inputs.json` 的 `archive_reference` 引用 `references/sources.tsv` 的固定官方 Linux v6.6 archive
  导出完整旧 UAPI。v6.6 对应 commit
  `ffc253263a1375a65fa6c9f62a893e9767fbebfa`，archive SHA-256 为
  `d926a06c63dd8ac7df3f86ee1ffc2ce2a3b81a2d168484e76b5b389aba8e56d0`。
  来源为 `https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz`；
  2026-09-16 核对官方 `sha256sums.asc` 与 Torvalds Git 的 peeled tag。
  该环境位于 `build/program-environment/busybox-environment/`。
- `build_busybox()` 从 `inputs.json` 选择固定 BusyBox 源码与配置，以 Git archive
  提取，再用 musl 静态链接。固定比赛 commit
  `b5ec6ef8497e1818cbdec3b54bb722f036e57972` 包含 BusyBox 1.33.1；
  原 `config/busybox-config-riscv64` 启用 398 applets。

两个 `prepare` 结果都返回 `include`、`cflags`、工具与源码身份、头文件树哈希。
调用方必须使用返回的完整 `cflags`；目前由 `-idirafter` 添加目标 UAPI，
并在编译器支持时使用 `-fno-link-libatomic`，避免发行版交叉 GCC 自动加入
不存在的 `atomic_asneeded`。musl 自带头仍先于 Linux UAPI。

Linux 7.2 UAPI 已移除 CBQ 定义，而固定 BusyBox 的 `tc` 仍引用这些定义。
完整 6.6 UAPI 是其编译环境；运行 Linux 内核继续使用固定 7.2 commit。
这里不混搭单个历史头，也不为通过构建裁剪 `tc`。这只建立编译兼容性，
不表示 BoarOS 已支持这些 applets 所需的全部系统调用。

## 失败、缓存和证据

固定源码 HEAD 或内容不符、archive 哈希错误、缺少工具、头文件导出或程序构建失败
均抛错。BusyBox `oldconfig` 若改变任一功能设置也失败；自动生成日期不参与此比较。
无 smoke 回退。`environment.json` 记录源码、工具及生成头的 SHA；只有身份与头文件树
均匹配才复用，重建旧 UAPI 时重新提取校验过的 archive。

构建期间 `full-busybox/` 保存 `source.tar`、原配置、`competition.config`、configure/build
日志、产物及 `build.json`；清理后只保留跨轮复用的源码构建树、配置、产物和身份文件。Linux 与 BusyBox 的源码许可证仍保留在各自固定来源或构建树中；
校验后的 archive 保存在 `references/linux-uapi/linux-6.6.tar.xz`；临时解包源码和构建产物位于忽略的 `build/`，不复制进内核源树。

## 验证

需要 `make`、宿主 GCC、RISC-V Linux GCC/binutils、`rsync`、`tar`、`curl` 与
已构建的 musl 工具链。首次准备旧 UAPI 需下载固定 archive。

```sh
make test-program-environment-host
make prepare-program-environment
make build-competition-busybox
```

host 测试保护源版本拒绝、头文件身份变化、配置功能不被裁剪及失败传播。
真实构建还编译含 musl、Linux 和 RISC-V `asm` 头的静态 probe；完整 BusyBox
产物必须交由 inventory 的 Linux/BoarOS 运行步骤继续验证。

## 客体内离线 C 编译探针

`tests/workloads/toolchain/offline_c.c` 是固定 Linux 与 BoarOS 共用的静态
musl `/init`。它从 `/work/tools.conf` 读取客体内编译器、汇编器绝对路径，
依次执行 `-E` 预处理、`-S` 编译、`as` 汇编、静态链接以及运行新 ELF。
每步把退出码、信号或 `execve` errno 写入 `/work/stages.tsv`，失败后不执行
依赖它的后续阶段。产物和目录均 `fsync`；宿主先重放 ext4 journal，再从
两个独立镜像提取 `.i`、`.s`、`.o`、ELF 和程序输出，逐项比较 SHA-256、
最终输出及文件系统检查。异常退出留下的 ext4 orphan 仅在严格核对修复项后
按下次挂载语义清理；其他 fsck 修复仍使测试失败，细节见
[离线工具链学习记录](../learning/offline-toolchain-probe.md)。版本字符串和目标文件存在本身不构成成功。

`make test-offline-c-baseline-riscv` 是**诊断模式**：该镜像故意不安装客体编译器，
同一 ELF 在两侧均应准确停在 `preprocess:exec:2`。它通过只说明探针可定位
首个失败，不说明离线编译已可用。`make test-offline-c-riscv` 是**严格模式**，
先验证并解包固定 Alpine v3.22 riscv64 APK 闭包到忽略的
`build/offline-c/alpine-tree/`，再要求五阶段全部退出 0、产物齐全且哈希
双侧一致、运行输出逐字节匹配；缺少固定编译器输入即失败。
`OFFLINE_C_LINUX_KERNEL` 可指向由固定 Linux commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e` 构建且有 `identity.json`
的缓存 Image；不指定时 runner 按固定资料构建。`make prepare-offline-c-toolchain`
单独重建编译器树。`references/sources.tsv` 对以下16个APK 固定逐项完整 URL
和 SHA-256，访问日期为 2026-09-27；准备脚本逐项校验 archive 哈希、
`.PKGINFO` 的名称/版本/架构/许可和缓存树内容，不修改上游包。

runner 从同一稀疏 ext4 fixture 建立 Linux 与 BoarOS 的独立原始镜像，
复制时保留空洞，避免为零区分配和写入宿主空间；QEMU 启动前同步 fixture
与各副本，使镜像准备的写回成本单独计入准备阶段。预同步不代替客体块驱动
当前30秒的有限请求期限，也不改变镜像内容、后续 journal 重放及产物比较
规则。runner 记录副本的逻辑/占用字节和复制同步耗时供 CI 诊断，不设墙钟
性能门槛。

| APK 包名 | 固定版本 | `.PKGINFO` 许可 |
|---|---|---|
| make | 4.4.1-r3 | GPL-3.0-or-later |
| binutils | 2.44-r3 | GPL-2.0-or-later AND LGPL-2.1-or-later AND BSD-3-Clause |
| gcc | 14.2.0-r6 | GPL-2.0-or-later AND LGPL-2.1-or-later |
| gmp | 6.3.0-r3 | LGPL-3.0-or-later OR GPL-2.0-or-later |
| isl26 | 0.26-r1 | MIT |
| jansson | 2.14.1-r0 | MIT |
| libatomic | 14.2.0-r6 | GPL-2.0-or-later AND LGPL-2.1-or-later |
| libgcc | 14.2.0-r6 | GPL-2.0-or-later AND LGPL-2.1-or-later |
| libgomp | 14.2.0-r6 | GPL-2.0-or-later AND LGPL-2.1-or-later |
| libstdc++ | 14.2.0-r6 | GPL-2.0-or-later AND LGPL-2.1-or-later |
| mpc1 | 1.3.1-r1 | LGPL-3.0-or-later |
| mpfr4 | 4.2.1_p1-r0 | LGPL-3.0-or-later |
| musl | 1.2.5-r12 | MIT |
| musl-dev | 1.2.5-r12 | MIT |
| zlib | 1.3.2-r0 | Zlib |
| zstd-libs | 1.5.7-r0 | BSD-3-Clause OR GPL-2.0-or-later |

小探针证明此固定C源码的五阶段；中等工程流程见下节。C++、Rust或持续本机自举仍需独立验证。
原版 libc-test `functional/socket.c` 的静态和动态 entry 可单独验收：

```sh
python3 tests/program-inventory/run.py --suite libc \
  --case libc.static.socket --case libc.dynamic.socket \
  --require-pass --output build/socket-program-check
```

此入口使用固定 `references/oscomp-testsuits` commit
`8b58dd16d26d30f7c74d48d5832d870d3051b703` 的未修改源码，
同一 ELF 分别在固定 Linux 与 BoarOS 的独立镜像执行，并要求原始输出、退出状态与
环境准备同时通过。复用已核验的构建缓存时可加 `--reuse-builds`。

## SQLite 日志与 NBD 故障入口

`references/sqlite/sqlite-amalgamation-3530400.zip` 是官方 SQLite 3.53.4 amalgamation，`references/sources.tsv` 固定下载 URL、2026-09-27 访问日期及 SHA-256 `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d`。构建直接解包到 `build/riscv/sqlite/`，不修改上游源码；复用 musl 1.2.5 工具链，保留线程支持、WAL 编译能力及原生 Unix VFS。回滚负载实际检查版本、`THREADSAFE=1`、VFS 名和 `journal_mode=DELETE`、`locking_mode=NORMAL`、`mmap_size=0`、`synchronous=EXTRA/FULL` 返回值。静态与动态 CLI 的可执行文件由同一固定源码编译。

`make test-sqlite-rollback-riscv` 在真实 U-mode 覆盖建表、提交/回滚、独立进程 writer 冲突、未提交大事务进程直接退出后的同次启动 hot journal 恢复、关闭重开，以及两种 CLI 对同盘数据库的 `integrity_check`。`make test-sqlite-nbd-riscv` 把同一负载接入宿主 C NBD 服务。`make test-sqlite-recovery-riscv` 以同一静态 ELF 在固定 Linux `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 与 BoarOS 执行 setup/mutate/recover；BoarOS 的 `EXTRA`、`FULL`、热日志与已确认提交路径经 NBD 重启两次，并用 `e2fsck -fn` 核对 ext4。小缓存 24 行事务制造 journal spill；恢复程序逐字节检查每行值、整事务状态和 `integrity_check`。

`make test-sqlite-wal-riscv` 用同一静态 ELF、官方 Unix VFS、`journal_mode=WAL`、`locking_mode=NORMAL`、`mmap_size=0` 和 `synchronous=FULL` 分别在上述固定 Linux 与 BoarOS 启动两次。首次启动核对版本与 PRAGMA、独立进程 writer 竞争、先后提交及另一进程持有未提交事务直接退出；关闭重开与第二次启动都检查三条已提交记录、未提交记录缺席和 `integrity_check`。该入口证明普通多进程 WAL 的组合 ABI，不替代 NBD 断电故障矩阵或实板持久性验证。

`make test-sqlite-wal-recovery-riscv` 与 `make test-sqlite-wal-recovery-matrix-riscv` 复用回滚日志的同一静态 ELF 和 NBD runner，通过客体 `/journal` 选择 WAL；启动核对实际 `journal_mode`，分别执行 EXTRA/FULL 正常事务、未关闭进程的 hot 状态、已确认提交、FULL 写/flush 失败及逐事件断电/错误矩阵。断电先冻结 NBD，再终止 QEMU，从稳定镜像启动新客体恢复两次，并运行 `e2fsck -fn`。恢复检查整事务旧值或新值、已确认提交保留新值、`integrity_check`；此入口仍只证明隔离镜像上的既定故障模型。

SQLite 大事务需要 Unix VFS 探测临时目录。固定 `build/riscv/sqlite/sqlite-amalgamation-3530400/sqlite3.c::unixTempFileDir` 用 `access(path, W_OK|X_OK)`；固定 `references/musl/musl-1.2.5.tar.gz`（SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`）将其转为 RV64 `faccessat` syscall 48。BoarOS 之前返回 `ENOSYS`，使 SQLite 在 journal spill 时返回 `SQLITE_IOERR_GETTEMPPATH`。当前单用户 root 模型提供路径存在性、普通文件执行位和只读挂载写入检查；完整多用户凭据权限仍未实现。`tests/diff-abi/access.c` 用固定 Linux 同一 ELF 验证该 raw ABI 的路径、mode 和 fault 边界。

`tests/host/nbd_fault.c` 经 Unix socket 给 QEMU 提供 fixed-newstyle、simple replies 的 READ/WRITE/FLUSH/DISC，只宣告 `HAS_FLAGS|SEND_FLUSH`。`tests/host/block_fault.c` 是 512 字节原子写、易失可见内容、稳定镜像的真实故障模型；只有成功 FLUSH 承诺此前事件持久化。`make test-nbd-host` 检查握手、读写、写/flush 失败和丢失、部分保存、反序保存。`make test-sqlite-recovery-matrix-riscv` 使用串口与宿主信号握手，仅对 SQLite 事务内的 NBD 事件编号，逐个注入写/flush 错误和断电位置；断电先冻结服务，再杀 QEMU，从同一稳定初态恢复。断电切点的事件上限由与该次运行相同的 none/odd/reverse 持久化策略分别探测；默认策略探针只用于写/flush 故障点，不能把它的事件数借给其他策略。2026-09-27 的 DELETE 小事务有 100 次写和 47 次 flush；441 个断电组合与逐位置 147 个写/flush 失败均通过。QEMU 基线为本地 `references/qemu/` commit `84f07211cc5b4fc6a371559bf8a5de4fb068e648`，执行环境为 QEMU 11.1.1。这些入口使用隔离镜像，成功后清理一次性目录；未确认提交只允许完整旧值或完整新值，已确认提交必须保留新值。不证明实板持久性。

## glibc 2.44 独立矩阵

`tests/userland/glibc/inputs.json` 固定宿主 RV64 GNU 工具链的 GCC、assembler、linker，以及未经修改的 `/usr/riscv64-linux-gnu/lib/ld-linux-riscv64-lp64d.so.1` 和 `libc.so.6` 的绝对路径与 SHA-256。loader 为 `f7c08812fe4e07dab8c3895ec3134a9c6695bfd1ece495530b280c1edfc11edb`，libc 为 `4103e7ae1d355639a116bd5393fd9ba03c6a3cd78b8e08bafd3fe9642c6fe9f5`；libc 内版本字符串为 2.44。身份不符即失败，不以另一个宿主 glibc 代跑。固定源码参考为 `references/glibc/glibc-2.44.tar.xz`，SHA-256 `37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667`；官方 URL 和 2026-09-27 访问日期见 `references/sources.tsv`。源码归档用于 ABI/许可证分析，不代表已证明宿主二进制从该归档逐位重建。

`make test-glibc-riscv` 编译同一源的静态 ET_EXEC、动态 ET_EXEC、动态 PIE、静态 PIE 和动态 pthread PIE，以及独立 TLS DSO；各 ELF 保留 GNU 编译器默认启动对象和 glibc，不修改二进制绕开内核。runner 将同一 ELF、loader/libc/DSO 分别放入 Linux 和 BoarOS 的隔离 ext4 镜像，使用固定 Linux commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`。它按 constructor/main、运行时、`atexit` 和 BoarOS 资源回收逐段判定失败。组合探针覆盖初始 TLS、`dlopen` 后主线程与新线程的独立 TLS、`pthread_create/join`、信号 handler 和退出；输出身份保存于 `build/riscv/glibc/identity.json`，成功后的运行镜像自动清理。构建命令为 `make test-glibc-riscv`。这只是固定版本上的已列举 ABI 子集，完整 glibc 应用及离线客体内编译仍待独立验证。

glibc 2.44 的 `pthread_join` 通过 `FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME` 等待线程退出；初始试跑在 BoarOS 返回 ENOSYS，glibc 因意外 futex 错误退出。`tests/diff-abi/futex_shared.c` 的同一 ELF 现在对照固定 Linux 检查 bitset 零值、绝对时钟、用户 fault、按掩码唤醒与 requeue；`tests/userland/pthread.c` 进一步检查 stop/continue 重启保留掩码与原截止时刻。当前内核 realtime offset 启动后不变，可一次换算为 monotonic；引入调时 syscall 时需重新处理阻塞中的 realtime deadline。

## 双侧执行与失败所有权

`make inventory-userland-riscv` 调用 `run.py` 构建上述完整 BusyBox 和
`libc_build.py` 的固定 libc-test，再经 `suites.run_suite()` 逐例启动两侧 QEMU。
`--reuse-builds` 显式复用已核验的 revision 和 ELF/DSO 校验值；默认重新构建。
`--case`、`--suite` 缩小运行集合；`--require-pass` 对本次所选集合要求每项完成且通过，未选项保持历史结果或 `not-run`。未指定 `--case` 时全量 228 项均需完成且通过。执行状态保存本次 `selection`，恢复运行可改变集合而不把未选项伪装成通过；runner 中断、环境错误、参考侧失败与未知案例仍为失败。
`--output` 指定本次核对目录，默认 `build/program-inventory-full`。运行期间默认在每个案例结果、日志、输出及其目录完成 `fsync` 后删除已通过案例的三份可重建磁盘；失败案例磁盘和 suite 基础 fixture 暂存，以便当轮诊断。诊断时可用 `--keep-pass-images` 保留新执行案例的全部磁盘。核对后用 `python3 tests/prune-build.py` 预览、`make prune-build` 清理整个一次性运行目录和旧缓存。

suite identity 包含程序/驱动/内核/执行器/校验器身份、QEMU 与磁盘工具身份和超时配置。
镜像由同一 fixture 独立复制；每例持久化结果后才继续。中断留下显式未运行项；恢复前核对完整
身份，输入变化要求新输出目录。构建或执行器异常直接失败，没有精简程序的自动回退。

`/init` 驱动准备 proc、sysfs、tmpfs、mqueue 和 loopback，父进程监督真实子程序。
stdout/stderr 分开保留，子程序的文本不能伪造驱动边界。驱动分别记录 setup 与 exec errno，
以及 wait status、终止信号和超时；超时杀死并回收子进程，外层另有启动/客体退出超时。
Linux 环境准备失败使该例成为无效参考；BoarOS 缺能力的诊断保留，并继续执行不依赖它的程序。

`reports.py` 校验上游 BusyBox/libc 脚本的完整有序断言，shell 退出码不足以证明通过。
只有参考有效且双方退出状态、完整原始输出一致才记为 `pass`。suite 的 `complete` 仅表示
manifest 所有项目完成；清单本身不是必过测试。原始命令、日志、输出及失败案例磁盘保留；通过案例的磁盘可从固定输入和基础 fixture 重建。不能因
环境或参考失败把 BoarOS 标为通过。具体固定输入及证据边界见
[真实程序清单](../learning/user-program-inventory.md)。

公共镜像提供RTC的10:135别名和根块节点252:0。原BusyBox包装器显式按全部逐项
contract和wait status比较，保留原输出及raw_output_equal；日期、内核日志、PID和容量
随系统变化，不能据字节不同虚构程序失败。其他案例默认字节比较不变。独立
`busybox.environment`核对日志-r/-c/-n、真实RTC内容和df/statfs同客体一致性。
分类证据及后续应用边界见[程序清单](../learning/user-program-inventory.md#当前基线与口径)。

## 原版 Lua 工程

固定Lua5.4.3（MIT）与GNU make4.4.1-r3，GCC14.2.0-r6及Alpine依赖保持原包。
`tests/offline-c-riscv.py --project lua`使用原Lua构建规则和默认FIFO jobserver；
`make test-offline-project-riscv`执行干净-j1、无变化、单源增量、clean重建、语法错误与
恢复、-j2及产物运行。Lua/luac、静态库嵌入及C共享模块由客体原GCC生成和实际执行。
递归jobserver另用共享计数核对两份资格和令牌归还，中断后检查子进程与临时FIFO清理。

`--performance --repeat 3`仅测独立启动的干净-j1/-j2和产物验证/收尾；
`make test-offline-project-tmpfs-riscv`是一次相同工作量的tmpfs归因对照。
`--observe --only boaros`使用默认关闭的COST构建；输出只留忽略的build。
程序、同步、根卸载分别计时，卸载使用已有ROOT_DRAIN_FIXTURE，不增加用户ABI。
固定Linux的PID1退出以panic停机，不能把它等同于BoarOS的根卸载；离线检查先重放日志。
输入与输出身份在机器清单校验，人类结果和解释归[离线工具链记录](../learning/offline-toolchain-probe.md)。
