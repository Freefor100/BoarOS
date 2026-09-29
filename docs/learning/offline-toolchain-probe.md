# 客体内编译的可观察边界

宿主交叉编译器生成 RV64 ELF，只证明宿主工具链和客体执行路径；验证客体内
离线编译必须把**可在 RV64 客体内执行**的编译器、汇编器、链接器、libc
开发文件和依赖安装进同一离线磁盘。探针按预处理、编译、汇编、链接、
运行分别记录真实子进程状态；若某步失败，后续阶段标记 skipped，避免
“找到 object 文件”掩盖编译或链接失败。严格模式要求同一输入在固定 Linux
与 BoarOS 的阶段状态、SHA-256 和最终程序输出一致。

固定 Linux 在 `/init` 退出后以 panic 结束客体，不走正常卸载。即便客体
对新产物及其父目录调用了 `fsync`，ext4 的目录项仍可能只在 journal 中；
直接用 `debugfs` 读原始镜像会误报文件不存在。宿主检查前先用
`e2fsck -E journal_only -y` 重放，再用 `e2fsck -fn` 核对，且只对该次运行
的镜像做此操作。GCC 的临时文件在异常退出时还可能留在 ext4 orphan file：
`journal_only` 不做 orphan 清理，固定 Linux `references/linux/fs/ext4/orphan.c`
commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e` 则在下次读写挂载时
调用 `ext4_orphan_cleanup()`。runner 仅当 fsck 输出**只有** orphan inode
及其空闲块/inode 计数问题时允许一次显式修复，检查修复输出只含这些变化，
再执行 `e2fsck -fn` 要求完全干净；其他修复一律失败并保留镜像。这样模拟
下次挂载可见内容，不把 fsck 的任意修复当成程序成功。

2026-09-27 的无编译器诊断基线：固定 Linux
`references/linux` commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`
的 Image SHA-256 为 `7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`；
BoarOS Image SHA-256 为 `717e11b014a2de22165e9f6ffe7d86199c9da5c17937933808efcb02f62dec17`；
驱动 SHA-256 为 `7810210bfc8cc50c65923650633c84e7aea69b1dce24ce6a0ff14f1f04fa7713`；
QEMU 为 11.1.1。两侧的第一失败均为 `preprocess:exec:2`，其余四阶段
跳过，`stages.tsv` SHA-256 同为
`bc6803c7bab4a798c61cd77da971f93f99cb0cb5c57d3706f9f0aa9779bd2728`。
重建入口：`make test-offline-c-baseline-riscv`。这不是 C 编译通过的证据。

2026-09-27 的固定 Alpine GCC 14.2.0-r6 路线（15 个 riscv64 APK 的完整
URL、SHA-256 和许可见 `references/sources.tsv`、
`docs/modules/program-environment.md`）暴露两个通用 ABI 缺口：GCC 的 cc1
以 `O_NOCTTY` 打开 `/work/program.c`，BoarOS 原先在 flag 校验阶段返回
`EINVAL`；随后四个编译阶段与 Linux 的产物已逐字节一致，但 GCC 对链接
产物调用 `fchmodat`，原先 `ENOSYS` 使最后 `execve` 返回 `EACCES`。
按固定 Linux `references/linux/fs/open.c` commit
`f4cdf7ca9a1fdcca413157df19753f388a5a224e` 与 raw 差分，BoarOS
接纳 `O_NOCTTY`，在 fs context 中保存 umask，并通过 lwext4 活 inode
handle 事务实现 `fchmod/fchmodat`。路径专用 setter 不可行：已 unlink 的
fd 仍须修改原 inode；直接越过 lwext4 改 raw inode 又会绕开 journal、
ctime 和错误 owner。固定 Linux 的 `fchmod(pipefd)` 也成功，但 pipe
合成 inode mode/fstat 尚未实现；此点不在本次编译器负载覆盖内。

重建：`make prepare-offline-c-toolchain`，随后
`make test-offline-c-riscv OFFLINE_C_LINUX_KERNEL=<固定 Image>`；不提供
Image 时按固定 Linux 来源构建。严格运行的五阶段退出码两侧全为 0，
`.i/.s/.o/ELF/output.txt` 的 SHA-256 各自双侧相同；生成 ELF 为
`9eb903417c06855766559ca00af19529ee85aa8a35973c2840de6ec6655e2697`，
stdout SHA-256 为
`a3f7bf4004ee05dee3c87923426f96984cb656d08bc17301a18c78910448c37f`。
Alpine 展开树 SHA-256 为
`ce84a7bb9fc7c97552121b37238622bbefbd4a3672600b57374e25230c582a07`；
固定 Linux Image SHA-256 为
`7ca338ec75e681cc68c5d946b3ae633fc0088fd78569b7847528105a9de6c8ec`。

主分支整合 glibc/futex 后的重建再次通过 `make test-offline-c-riscv`：
BoarOS `kernel-rv` SHA-256 为
`9f848c4b74aa8415c0869616abfccd456e26742e1959d717b7f59f57f50164c4`，
固定 Linux Image SHA-256 为
`16a93ddb1d451898b93fff14de0cc076bcf1b10dad54c19a3e179a6cd81103b1`，
驱动 SHA-256 为 `7810210bfc8cc50c65923650633c84e7aea69b1dce24ce6a0ff14f1f04fa7713`，
输入 C 源 SHA-256 为 `5f3226afadc0a75dc9a9692baa7428fca406c00c65711b786b0994d94caf5beb`。
五阶段均为 `exit:0`；产物哈希仍与上段一致。固定 Linux 异常退出产生的
orphan 文件在审核后清理，最终 `e2fsck -fn` 完全干净。

2026-09-28 通用 VFS/procfs 阶段复验 `make test-offline-c-riscv`：
BoarOS 内核 SHA-256 `1a0dc5b9dc338e01d9fc7b10c689edaaa761f75952bc8fce90f2f4a4c1478167`，固定 Linux Image SHA-256 `09aef347ca137306aa97c9b7a87ec464bae1097011ce15f08b91529e114f8b5c`，Alpine 展开树 SHA-256 `ce84a7bb9fc7c97552121b37238622bbefbd4a3672600b57374e25230c582a07`。预处理、编译、汇编、链接、运行五阶段两侧均 `exit:0`；生成 ELF 与输出 SHA-256 保持上述固定值，未出现回退。

2026-09-29 [CI run #36](https://github.com/Freefor100/BoarOS/actions/runs/36513240987) 的 SQLite DELETE/WAL 均通过，离线 GCC 却在 `/init` 执行前的根盘启动过程中报 VirtIO 一秒 timeout。失败日志只能证明设备请求超时，不能据此判定 ext4 或 SQLite 回归，也未打印出请求类型。该 fixture 逻辑大小 556598166 字节、实际分配约 248 MiB；原 `shutil.copyfile` 把每个启动副本扩成约 531 MiB，两侧启动前增加约 600 MiB 无意义的宿主写入。本地旧 QEMU 8.2.2 在 25% 和 5% CPU 配额下各 20 次根启动均通过，单纯 CPU 配额未复现 CI 的块延迟。runner 现使用保留稀疏空洞的复制并在启动前 `fsync` fixture 和副本，减少宿主回写与客体首次请求竞争；逐字节 `cmp` 已确认副本内容不变，内核一秒超时也不变。修正后的本地 QEMU 11.1.1 双侧五阶段通过，QEMU 8.2.2 上 BoarOS 单侧五阶段连续 10 次通过；该环境中的本地缓存 Linux Image 在旧 QEMU 上出现 cc1 SIGILL，不能拿它做双侧对比。远端间歇性故障是否消除仍须新 CI run 验证。
