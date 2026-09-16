# RISC-V Linux differential ABI

`make test-diff-abi-riscv` 将同一个无 libc raw-syscall ELF `/init` 分别交给
Linux 与 BoarOS 生产内核。两次 QEMU system 均为 `virt`、一个 hart、512 MiB，
使用同一 raw whole-disk ext4 fixture 的独立可写副本。Linux 内核来自
`references/linux`，commit `f4cdf7ca9a1fdcca413157df19753f388a5a224e`，
版本由 `references/sources.tsv` 的唯一 Linux 项提供；没有写死 reference 输出。
Linux 原有源码许可证见 `references/linux/COPYING`，构建产物位于被忽略的
`build/diff-abi/`，不复制 Linux 源码进入 BoarOS。

## 入口与契约

- `tests/diff-abi/cases.c`、`start.S`：同一 RV64GC/LP64D 程序调用 RISC-V
  Linux syscall ABI。覆盖 open 的三种访问模式、read/pread/file-private mmap、
  sparse hole/尾页零填充/越过 EOF 的 SIGBUS，以及 write/writev/append 的
  0、1、32、63、64、65 字节有效用户前缀。请求长度统一为有效前缀 + 1，
  文件初始位置为 1；该参数契约保存在 `cases.txt` 并记录其 SHA-256。
- `abi.h`：case pack 公共 syscall、stat、输出与 setup 检查。`truncate.c` 提供驻留页/COW/PROT_NONE、当前与非当前 MM、关闭 fd/unlink、
  非对齐尾页、O_TRUNC 与重新增长的观测；记录 ID 同步维护在 `cases.txt`。
- `timestamps.c`：22 条 create/read/write/truncate/unlink 与父目录时间观测。Linux 根挂载沿固定源码 `fs/namespace.c` 的默认 `MNT_RELATIME`，BoarOS 使用同一策略。
- `readv.c`：覆盖向量导入顺序、零项及零长度、1024/1025 项、`MAX_RW_COUNT`、普通文件跨页/共享 OFD/EOF、0/1/32/63/64/65 字节可写前缀，以及 pipe 的未完成片段保留、写入故障、尾片段合并、环回、非阻塞/EOF 和可写 poll 边界。与上述测试合计 126 条记录；不以手工 Linux 输出替代双侧执行。
- `harness.py`：构建 Linux、制作镜像、运行两个系统、校验完整协议并做严格 diff。
- `linux.config`：以 `allnoconfig` 为基础，启用 virt、MMU、ELF、串口、VirtIO
  MMIO/block、ext4 和关机所需能力；Linux 自己解析依赖。

每条记录含 ID、返回值、errno、文件 size、OFD offset、子进程 wait status 和
完整观测数据的十六进制值。fd 与 mmap 地址只有其成功身份规范为 0；错误返回、
部分计数、字节、大小、位置和退出/信号状态均保留。未观测 size/offset 用 -1。
除时间观测外，规范化只去除引导噪声和串口 CRLF；缺失、重复、未知、乱序、格式错误的记录，
错误 errno、缺少 END、QEMU 非零退出和超时均失败。BoarOS 还必须报告 PID 1
状态 42、heap-live 为零及完整资源回收关机。

时间观测保留原始 224 字节（28 个有符号 little-endian RV64 word）：操作前后时钟区间、文件操作前后的 atime/mtime/ctime、父目录操作前后的三个时间，均以秒/纳秒成对编码。`timestamps.py` 校验纳秒范围、变化字段属于操作时钟区间，再比较字段变化关系、创建时间相等关系、mtime/ctime 关系及 relatime 前置条件；不直接比较两次独立启动的绝对墙钟。规范化器自身的哈希进入运行清单，原值仍保存在串口日志。宿主测试故意破坏纳秒、时钟范围和变化关系以验证其会失败。

Linux PID 1 在完整输出后 sync 并 reboot poweroff；同一程序在 BoarOS 的未实现
调用返回后 exit(42)。这些启动/终止适配不参与案例观测，不影响文件操作路径。
fixture 内 `/dev/console` 是由 debugfs 创建的字符设备 inode，用于 Linux init
标准流；无需宿主 root 或 loop mount。

## 构建、缓存与证据

依赖 GNU Make、RV64 bare-metal GCC/binutils、RV64 Linux GCC/binutils、宿主 GCC、
flex、bison、bc、Python 3、QEMU system RISC-V、OpenSBI、e2fsprogs 和 Git。
Linux 源码缺失时只 fetch manifest 中的固定 commit；现有源码有修改、版本或
origin 不符时失败。源码构建时，已有 sparse checkout 按参考资料流程展开同一 commit；缓存命中时
直接验证已记录的 Image 和 config，不读取 Linux 工作树。

```sh
make test-diff-abi-host
make build-diff-abi-linux
make test-diff-abi-riscv
```

缓存 key 包含固定来源/commit、输入 config、构建器代码、编译器/汇编器/链接器
以及宿主构建工具的版本与可执行文件 SHA-256。读取缓存还验证实际 `.config`
和 Linux Image SHA-256。`LINUX_CC` 可选择以 `gcc` 结尾的交叉编译器，
`DIFF_JOBS` 控制 Linux 构建并行数。

`build/diff-abi/run/` 保存双方 raw logs、normalized records、`results.diff` 和
包含内核、程序、fixture 哈希、工具版本及完整 QEMU 命令的 `metadata.json`。
启动 Linux 构建前即快照 BoarOS 内核、测试 ELF 和案例清单，哈希及运行均使用
该快照，避免并行构建改变输入。成功时删除运行镜像，失败保留三份初始/运行镜像。每次运行替换此目录，需要长期
保存的证据应在下一次运行前自行复制。CI 使用独立 job、精确 Linux cache key，
始终上传日志，失败时另传镜像。

宿主协议测试包含刻意错误返回值/errno、数据、size、offset、signal，缺失/重复
结果和实际子进程 timeout/nonzero，确保比较器不会将未执行或不完整运行当作成功。
此入口验证明确案例子集，不代表完整 Linux ABI 或尚未实现架构通过。

## 固定 reference 的已知限制

在上述固定 Linux、同一最小 config 与 4 KiB ext4 上，请求 128 字节的跨页
无效用户缓冲区测试发生真实超时。文件位置 0 与 1 均复现；GDB 均停在
`handle_exception <- fault_in_iov_iter_readable`，位置 1 时已经复制 31 字节，
剩余 97 字节的源地址落在有效页最后一字节。
本地 `references/linux/arch/riscv/lib/uaccess.S` 的 word/shift-copy 可能在
跨页 load 上无法复制剩余有效字节，而 `references/linux/mm/filemap.c` 的
`generic_perform_write()` 在 prefault 仍发现有效字节后继续 retry；这是结合
源码与调试现场的原因分析，并非修改过的 Linux 行为。

当前 pack 使用统一的最小 fault span（有效前缀 + 一个无效字节）保护部分
成功的 ABI 契约；不把上述 128-byte 实例超时规范化为成功，也不更换固定
Linux commit。较长请求的上游 RISC-V usercopy 进展问题留作独立 reference
调查。SIGBUS 对照显式禁用 Linux core dump，仍严格比较整个 wait status，
不能把没有生成 core 的退出伪装为 `WCOREDUMP`。
