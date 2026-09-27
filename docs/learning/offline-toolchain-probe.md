# 客体内编译的可观察边界

宿主交叉编译器生成 RV64 ELF，只证明宿主工具链和客体执行路径；验证客体内
离线编译必须把**可在 RV64 客体内执行**的编译器、汇编器、链接器、libc
开发文件和依赖安装进同一离线磁盘。探针按预处理、编译、汇编、链接、
运行分别记录真实子进程状态；若某步失败，后续阶段标记 skipped，避免
“找到 object 文件”掩盖编译或链接失败。严格模式要求同一输入在固定 Linux
与 BoarOS 的阶段状态、SHA-256 和最终程序输出一致。

固定 Linux 在 `/init` 退出后以 panic 结束客体，不走正常卸载。即便客体
对新产物及其父目录调用了 `fsync`，ext4 的目录项仍可能只在 journal 中；
直接用 `debugfs` 读原始镜像会误报文件不存在。宿主检查前需先用
`e2fsck -E journal_only -y` 重放，再用 `e2fsck -fn` 核对，且只对该次运行
的镜像做此操作。该步骤模拟下一次挂载所见内容，不把 fsck 的普通修复
当成程序成功。

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
