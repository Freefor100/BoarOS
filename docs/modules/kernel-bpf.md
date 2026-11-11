# BPF 网络过滤与系统调用跟踪

RV64 与 LoongArch64 共用 `kernel/bpf/` 的对象、校验器、解释器、map 和跟踪实现。
Linux ABI 以 `references/sources.tsv` 固定的 Linux 为依据；原版工具的版本、归档和
校验值也由同一清单维护。支持范围围绕 bpftool、libbpf 的 UDP 过滤程序和
bpftrace 的 `raw_syscalls` 跟踪选择。

## 对象、接口与权限

`bpf(280)` 提供程序/map/BTF/link 的创建或加载、信息查询、ID 枚举和按 ID 打开，
以及 map 元素与批量操作、freeze、pin/get 和程序测试执行。FD/OFD 持有对象；
程序持有它引用的 map 和 BTF，socket 或 perf 挂载持有程序，bpffs 节点持有被 pin
的对象。关闭一个 FD、替换过滤器或 unlink pin 只解除对应的使用者；最后一个
实际引用消失才从 ID 索引移除并释放。FD 关闭与执行并发时，运行方仍持有引用。
`/proc/<pid>/fd` 和 `fdinfo` 展示实际 BPF 对象及其信息。

加载和特权对象操作检查 `CAP_BPF`，跟踪检查 `CAP_PERFMON`；对应接口保留
Linux 的 `CAP_SYS_ADMIN` 兼容授权。TCP socket 过滤遵循固定 Linux 的网络管理
权限要求。`/proc/sys/kernel/unprivileged_bpf_disabled` 初值为 2；有权调用者可
在 0 与 2 间切换，写入 1 后不可恢复。sysctl 的写入要求 `CAP_SYS_ADMIN`。

程序类型支持 `SOCKET_FILTER`、`TRACEPOINT`、`RAW_TRACEPOINT` 和为能力探测
使用的 `KPROBE` 加载。原始 tracepoint 程序用于 BEGIN/END 测试执行；真实挂载
由 tracepoint perf 事件完成。没有实现的命令、类型、helper 和挂载返回错误，
不以空对象或成功返回代替实现。当前没有 JIT、XDP/TC、AF_PACKET、seccomp、
硬件 PMU 或 kprobe/uprobe/fentry 挂载。

## 校验与执行

eBPF 解释器执行标准整数、端序、访存、跳转、本地调用和原子操作；cBPF 先经过
其控制流和初始化校验，再转换为同一执行器的指令。eBPF 校验器跟踪寄存器和
栈初始化、标量范围、指针来源与范围、可空 map 指针、上下文访问、helper 参数
以及 ringbuf 预留的提交/丢弃。控制流合并保留各路径必须满足的条件；有界循环
需要在加载时证明终止。每帧栈 512 字节，最多 16 个调用帧，验证复杂度上限为
1,000,000。日志返回实际失败原因及处理计数。只读且已经 freeze 的直接 ARRAY 值可以作为验证期常量读取；仍可由用户或程序改写的 map 不参与常量推导。

程序发布前完成校验、map 绑定和运行工作区准备。每个 CPU 使用本程序自己的
工作区；执行期间禁止迁移，不持有对象索引锁或跟踪登记锁运行解释器。
`PROG_TEST_RUN` 支持 socket filter 的 Ethernet 输入/L3 执行视图及 raw 程序的
上下文输入；后者必须覆盖程序实际验证过的最大上下文偏移。输出按 Linux 的
独立字段回写，较短的历史 attr 不要求输入区域也可写。

## Map、helper 与共享内存

支持 `ARRAY`、`HASH`、`PERCPU_ARRAY`、`PERCPU_HASH` 和 `RINGBUF`。普通和
per-CPU map 提供查找、更新、删除、枚举及相应批量操作；全局数据与只读数据
使用普通 ARRAY 和 freeze。hash 预分配存储；不支持的 NO_PREALLOC 等标志明确
拒绝。per-CPU 用户值按 8 字节对齐，以实际 possible CPU 数量排列。

程序运行借用 map value；hash 删除或替换将仍被借用的旧值留到执行结束再回收。
lookup-and-delete 在同一个 map 临界区取得值并删除。用户 FD 权限与 map 的
用户/程序访问限制分别检查；WRITEONLY_PROG 允许查找后写值，禁止读值。
批量修改先写出 count=0，再逐项导入和修改；后续复制故障保留实际完成前缀。

helper 包括 map 操作、单调/启动时间、PID/TID、UID/GID、comm、CPU、包读取和
ringbuf reserve/submit/discard/output/query。内存探测使用不等待的 nofault 访问，
不为探测触发文件缺页 I/O。ringbuf 采用 Linux 的 consumer/producer 共享页布局
和两次连续映射的数据窗口；记录发布与通知遵循 BUSY/DISCARD 状态，poll/epoll
使用实际 ringbuf 等待队列。ARRAY 全局值也支持 mmap。

map 存储拥有真实物理页；用户映射通过 OFD 持有 map，并直接引用这些页。
mmap、freeze、fork、mprotect、munmap 和远程 TLB 回收的规则见
[MM 的 BPF 共享映射](kernel-mm.md#bpf-对象共享映射)。

## Socket 与 syscall 事件

`SO_ATTACH_FILTER`、`SO_GET_FILTER`、`SO_DETACH_FILTER`、`SO_LOCK_FILTER`
和 `SO_ATTACH_BPF` 使用 socket 自身的程序引用。返回 0 丢弃，正数保留相应长度。
UDP/TCP 在协议状态变化前执行，保留协议处理所需的 UDP/TCP 头；包读取直接
跨 pbuf 片段，不建立完整包副本。TCP accept 继承过滤器及锁定状态；UNIX 的
DGRAM/SEQPACKET 在接收队列接纳前过滤，STREAM 依 Linux 语义不以该过滤器
改变接收结果。具体接收位置和回调所有权见[网络模块](kernel-network.md)。

tracefs 提供 `events/raw_syscalls/sys_enter`、`sys_exit` 的真实 ID 与 format。
架构异常边界调用共同跟踪入口，覆盖快速查询、普通调用、等待、clone/exec、
未知 syscall 和重启路径。exit 事件在信号处理和 restart 转换前记录返回值，
保留可观察的 `-ERESTART*`；没有返回的退出调用不伪造 exit 事件。

`perf_event_open(241)` 支持这两个 tracepoint 的标准 libbpf 绑定配置、启停和
绑定 ioctl，以及 perf-event BPF link。按固定 Linux，挂在同一 tracepoint 上的
BPF 程序都执行，perf 的 PID/CPU/启停条件控制该事件的计数；程序用自己的
PID/TID 条件筛选跟踪目标。现代 link 独立持有 perf/program，legacy 绑定在
perf FD 最后关闭时解除。运行方在登记锁内取得引用，锁外执行并通知等待者。

sysfs 提供真实 CPU possible/online 和 `/sys/kernel/btf/vmlinux`。内核构建从本次
BoarOS ELF 的 DWARF 生成 BTF，嵌入最终 ELF，再以只读文件页提供给工具读取
和 mmap；没有复制参考 Linux 的结构或伪造内核类型。

## 构建与检查

原版工具及其运行库构建在被忽略的缓存中，默认并行度为 2，客户机内存为 1 GiB。
`build.py --help` 列出组件选择；Linux 对照内核由同目录 `linux.py` 构建。

```sh
python3 tests/workloads/bpf/build.py --help
python3 tests/workloads/bpf/linux.py --help
make test-bpf-host
python3 tests/workloads/bpf/abi.py --arch riscv --cpus 1 2 8
python3 tests/workloads/bpf/abi.py --arch loongarch --cpus 1 2 8
python3 tests/workloads/bpf/baseline.py --arch riscv --platform boaros --mode full --cpus 2 --kernel build/riscv-user-smp8/kernel-rv
python3 tests/workloads/bpf/baseline.py --arch loongarch --platform boaros --mode full --cpus 2 --kernel build/loongarch-smp8/kernel-la
```

同 ELF 用例检查 syscall/setsockopt/perf 参数、复制故障、映射权限与清理，以及
真实 UDP/TCP/UNIX 行为。原版工具用例检查 bpftool 的 ELF/map/BTF/bpffs，
libbpf map 更新改变 UDP 收包规则，并让原版 bpftrace 在客体中编译脚本，统计
受控目标的 64 次 syscall、按线程配对耗时和 64 条 ringbuf 输出，再检查正常退出
及 SIGINT 的 END/清理。Linux 对照和 BoarOS 使用相同的工具、ELF 与运行库。
