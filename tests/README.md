# 测试目录

`make test-*` 按本次修改选择检查。目录按执行环境分开，用户程序按子系统组织；同一个夹具可供多个行为检查复用。

## 保留内容及用途

| 路径 | 保留的实际用途 |
|---|---|
| `host/` | 直接链接生产实现，控制 OOM、设备错误、复制故障与并发交错；检查真实引用归还、回滚和唯一完成。用户程序无法稳定命中这些内部失败点 |
| `host/allocator/`、`host/mm/`、`host/block_model/`、`host/sync/`、`host/random/` 等子目录 | 上述检查所需的架构替身、受控设备或配置头，供真实生产源编译；它们不是额外的一套功能实现 |
| `riscv/` | 保留真实页表、缺页/COW、TLB 退休、MM/uaccess、块设备 DMA/取消及文件失败路径的客机检查。宿主替身不能执行这些架构边界 |
| `loongarch/` | 保留 LA 页表、PCI/中断、异常与 FPU/SIMD、根盘及故障清理；同时提供固定工具和 Linux 参考的准备入口，不能用 RV 通过替代 LA 验收 |
| `common/` | 两架构实际共用的等待、锁、启动汇编和 ABI 类型；避免各架构复制相同夹具。旧参数包装自测已删除 |
| `diff-abi/` | 用同一 ELF 对照 Linux 的返回值、错误顺序和已发生的副作用；保留固定参考配置和实际 syscall 消费者 |
| `userland/` | 真实 libc、线程、exec、信号、凭据、FD 和 MM 组合；覆盖生产调用链，防止模拟分派器掩盖行为差异 |
| `userland/glibc/` | 固定 GNU 工具与运行库身份、动态装载和 TLS 消费者；验证原始 glibc，而非只验证无 libc 的 syscall 程序 |
| `userland/inventory/` | 固定原程序构建输入、清单和执行器；检查未经替换的真实应用，保持用户程序与内核错误分类 |
| `userland/tty/` | 真实终端交互、作业控制和阻塞取消，需要终端输入握手，不能由普通文件读写覆盖 |
| `smp/` | secondary 启停、每核执行、运行权交接、远程 TLB 确认与受控组合；只有真实多核能暴露这些交错 |
| `workloads/io/`、`workloads/network/` | 当前固定文件/TCP 工作量、向量/批量消息、复制和零拷贝消费者；内容核对、同步完成与吞吐比较复用相同程序 |
| `workloads/credentials/`、`resource/`、`signal/`、`mremap/`、`fs/`、`xattr/` | 当前权限、进程/MM/挂载、信号恢复及文件属性接口的 Linux 可观察语义；保留直接调用与生命周期反例 |
| `workloads/futex/`、`ipc/`、`locks/`、`pipe/`、`fifo/`、`epoll/`、`event/`、`smp/` | 原子操作、超时、中断、FD 复用、关闭与跨核通知；各自对象所有权不同，共用等待机制并不能替代对象自身的检查 |
| `workloads/sqlite/`、`toolchain/`、`tty/` | SQLite 恢复、客机内编译和终端等实际消费者；它们覆盖多个 syscall 的真实组合，不是为单个入口打印成功 |
| `workloads/bpf/` | 原 bpftool/libbpf/bpftrace 与同 ELF 原生 ABI 消费者；校验器、BTF、trace 和 mmap 必须同时与原工具兼容 |
| `cost/` | 仅保留现用 I/O/TCP/accounting 执行器、它们的参数/完成条件自测，以及被实际导入的快照解析、fixture、插件和共用头。旧独立校准器、重复报告验证与无人调用负载已删除 |
| `tools/` | 当前栈预算、ELF/CRC 布局、固定输入和构建清理；检查仍然存在的构建约束，不维护早期目录布局录制器 |
| 根目录 Python 模块 | 实际执行器共用的架构配置、绑核、测量互斥、种子、网络与 NBD 控制；减少每个 runner 重写启动和资源清理 |

具体行为以模块中的检查入口和测试源码为准；一个 shell/Python 文件可能负责编译、启动和结果检查，并不等于多个重复测例。

## 使用

```sh
make test-mm-pin-host
make test-page-cache-concurrency-host
make test-user-smp-riscv
python3 -B tests/userland/run.py --help
python3 -B tests/cost/io-budget-experiment.py --help
```

功能或实现变化后只运行对应检查。一次性诊断、单轮组合程序与中间报告放在忽略的 `build/`，不加入本目录。当前机制说明见 `docs/modules/`，仍在使用的测量方法见 `docs/learning/`。

## 构建产物

`make prune-build` 默认删除内核对象、测试 ELF、参考内核、运行盘、日志和实验目录。自动保留的只有仍在使用的已安装工具/SDK、LA RTC 模拟器可执行文件与身份记录，以及尚未提交源码的本地归档；活动 Git worktree 的源码另行保护。工作区内的生成盘和构建目录仍应单独清理，不能因 worktree 被保护就把产物永久保留。

清理前可用 `python3 -B tests/tools/prune-build.py` 查看列表；仅本轮尚在使用的输出才用 `--keep build/<目录>` 明确保留。固定输入由 `references/sources.tsv` 与准备入口重建，旧实验通过 Git 历史查阅。

`tests/userland/run.py --workload sendfile_tcp` 使用真实 AF_INET loopback TCP
消费 `sendfile`，检查跨页内容、显式/共享 offset、非阻塞短发送后续传、信号、
复位及最后发送进程退出后的所有权归还。同一连接上的并发 `send/write` 允许
片段交错但分别核对内容和顺序，并检查非最终关闭、取消后其他持有者继续发送、
最终 EOF，以及 pipe→TCP `splice` 的短发送、非阻塞与恢复。`--performance` 使用夹具预先写入的
8 MiB 固定文件，分别以 4 KiB 和 256 KiB 请求发送；接收者逐字节核对后应答，
固定发送/接收进程的 CPU，并分别记录发送接纳、完整消费和发送者 CPU 时间。
前后比较应复用同一 ELF/夹具、
同一 CPU/内存配置并错峰执行；该模式走内核 loopback，不代表物理网卡吞吐。
