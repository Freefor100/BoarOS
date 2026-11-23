# 存储与网络数据路径受控实验工具

性能优化离不开可重复、边界清晰的固定工作量基准。BoarOS 在仓库中保留了三套面向存储读写、目录枚举与网络协议栈的受控实验工具，用于独立复现与对比不同内核版本及编译期参数配置的表现。核心子系统设计见 [VFS 与 ext4 文件系统](../modules/vfs-ext4.md)、[VirtIO 块设备](../modules/riscv-virtio-block.md) 与 [网络协议栈](../modules/kernel-network.md)，整体性能工程方法论见 [BoarOS 性能工程与关键优化](../performance.md)。

---

## 一、存储数据路径实验（`io-budget-experiment.py`）

`tests/cost/io-budget-experiment.py` 驱动用户态程序 `tests/workloads/io/data-path.c` 与 `storage-path.c`，覆盖从内存文件系统（tmpfs）到磁盘 ext4 的全套典型 I/O 路径。

```sh
python3 -B tests/cost/io-budget-experiment.py profiles
python3 -B tests/cost/io-budget-experiment.py --help
python3 -B tests/cost/io-budget-experiment.py run --help
```

### 1.1 编译期窗口参数（`profiles`）

在 4 KiB 页配置下，内核默认的页缓存顺序预读窗口与脏页写回批次配置为 **`ra16-wb32`**（定义于 `include/kernel/page_cache.h`）：

- `BOAROS_PAGE_CACHE_READAHEAD_PAGES = 16`（64 KiB 顺序预读上限）；
- `BOAROS_PAGE_CACHE_WRITEBACK_PAGES = 32`（128 KiB 写回批次上限）。

实验脚本支持在 `ra{0,1,2,4,8,16,32}-wb{1,2,4,8,16,32}` 矩阵中自动构建候选内核，并将源码提交、编译器版本、实际生效宏定义与观测开关封装为构建清单写入内核镜像。实际落盘的块设备请求大小仍受底层物理区段连续性与分散聚集（Scatter-Gather，SG）表长度约束。

### 1.2 用例格式与完成边界

每条测例遵循六段格式 **`backend:operation:completion:files:bytes_per_file:request_bytes`**：

- **常规操作（`operation`）**：`append`（顺序追加）、`overwrite`（覆写）、`cold-read`（冷盘读取）、`hot-read`（预热读）、`read`（tmpfs 驻留内存读）、`parallel-append`（2 或 4 并发执行者并行追加）、`range-modify`（固定范围修改）与 `mixed-read`（由 1 个预先写脏数据的写者、2 个独立冷读者和 1 个高频 `open`/`close` 执行者并发组成，脏数据准备阶段不计入计时窗口）。
- **固定策略操作**：包括 `writev-{1,4,64}-{hot,cold}-{same,split}`、`sync-write`（区分 `osync`/`odsync`/`fsync16`/`fdatasync16`）、`hot-write`、`sparse-write`、`stream-write`、`periodic-sync`、`active-mixed`、`age-burst`、`age-hot`、`mmap-write`、`pwrite-pair`、`domain-idle` 与 `domain-busy`。
- **完成边界（`completion`）**：严格区分 `cache`（仅计写入页缓存接纳返回）、`fsync`（含完整数据与元数据落盘）与 `fdatasync`（仅同步数据及恢复所需元数据）。其中 `cold-read` 仅允许在预置未读数据的 ext4 镜像上运行，tmpfs 读取属于常驻内存读取，绝不与冷盘混淆；单进程多文件轮转与多进程并发追加属于不同工作量。

正式对比时显式传入 `--baseline` 与 `--variant label=kernel`（可用 `--program` 复用冻结 ELF）。执行器记录每文件实际字节、数据校验和、同步与请求耗时、进程 CPU 时间、处理器亲和性及资源归还状态，并在运行结束后执行磁盘文件系统一致性检查。

---

## 二、目录枚举基准（`getdents_batch.c`）

`tests/workloads/fs/getdents_batch.c` 专门用于衡量大目录扫描中系统调用批处理与用户态拷贝的摊薄效率：

```sh
getdents_batch DIRECTORY ENTRIES BUFFER_BYTES ROUNDS [prepare]
```

- **计时边界**：传入 `prepare` 时，程序先在空目录创建 `ENTRIES` 个固定编号文件（`%08u`）并执行 `fsync`，再执行一遍完整预热扫描以触碰用户页并填充目录缓存；创建与预热均不计入耗时。计时窗口仅涵盖 `ROUNDS` 轮 `lseek(0)`、`getdents64` 系统调用与文件名唯一性校验，输出总项数、返回字节数、系统调用次数、墙钟时间（`wall_ns`）及进程 CPU 时间（`cpu_ns`）。
- **对照设计**：这是一个明确的**暖目录基准**（不能当作冷盘结果）。典型输入为 `4096` 项分别搭配 `4096` 字节与 `32768` 字节缓冲区，再以 `32` 字节缓冲区（每次仅容纳 1 项，无法摊薄目录迭代器与用户拷贝固定成本）作为基线对照。在两份内核上复用同一 ELF、同一目录镜像与页大小，比较每秒枚举项数（`entries/s`）与每项 CPU 耗时（`CPU ns/entry`）。
- **并发与回退边界**：开启诊断时可另行统计 `ext4_dir_iterator_init`、`inode` 引用获取及 `kernel_copy_to_user` 次数；优化目录批读时还须单独测量并发写者的创建/删除延迟，防止批读持锁过长拖慢写入。此外，IOzone 面向文件内容读写而不涉及大目录枚举，不能拿 IOzone 分数证明 `getdents` 收益——仅在作为无关数据路径的回退检验时，保持原 IOzone ELF、记录大小、并发数与 `fsync` 选项一致，分别报告读/重读/写/重写成绩。

---

## 三、网络数据路径实验（`network-budget-experiment.py`）

`tests/cost/network-budget-experiment.py` 驱动 `tests/workloads/network/tcp-budget.c`，在独立启动的虚拟机中测试本机回环（`loopback`）与虚拟网卡（`tap`）下的传输控制协议（Transmission Control Protocol，TCP）吞吐、延迟与混合流共存表现。

```sh
python3 -B tests/cost/network-budget-experiment.py profiles
python3 -B tests/cost/network-budget-experiment.py --help
python3 -B tests/cost/network-budget-experiment.py run --help
```

### 3.1 协议窗口与内存池参数（`profiles`）

协议参数配置采用 `w{8,16,32}-p{1,2,4}-m{1,2,4}` 命名（定义于 `net/lwip_port/include/lwipopts.h`）：

| 维度 | 宏名称 | 默认配置（`w32-p4-m2`） | 早期基线（`w8-p4-m2`） | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| **窗口与发送额度（`w`）** | `BOAROS_LWIP_WINDOW_MSS` | `32` 个 MSS（46,720 B） | `8` 个 MSS（11,680 B） | 普通外部 TCP 连接的接收通告窗口与发送缓冲额度（MSS = 1,460 B）；回环连接保持 8,192 B MSS 与 32,768 B 窗口 |
| **协议池倍率（`p`）** | `BOAROS_LWIP_POOL_SCALE` | `4` 倍 | `4` 倍 | 缩放 TCP 段描述符（`128 × p`）、`pbuf` 头与缓冲池（`64 × p`） |
| **协议堆倍率（`m`）** | `BOAROS_LWIP_MEM_SCALE` | `2` 倍（512 KiB） | `2` 倍（512 KiB） | 缩放 `lwIP` 动态协议堆大小（`256 KiB × m`） |

> 活动 TCP 协议控制块（Protocol Control Block，PCB）上限固定为 32 个（监听 PCB 16 个、UDP PCB 16 个），不随候选配置放大。扩大内存池能缓解全局资源接纳压力，但无法消除单连接小窗口受往返时延（Round-Trip Time，RTT）限制的带宽延迟积上限，也不能替代多核连接归属分片与网卡完成处理优化。静态池数组体积与运行期堆峰值分开统计，避免用相近的动态堆峰值掩盖静态内存膨胀。

### 3.2 用例格式与隔离执行

用例采用五段格式 **`path:mode:kind:connections:direction`**：
- `path`：`loopback` 或 `tap`（每次运行创建全新独立网络命名空间；附加 `netem` 延迟时按实测回显 RTT 解释）；
- `mode`：`blocking`（阻塞）或 `nonblocking`（非阻塞）；
- `kind`：`rr`（请求响应）、`bulk`（大块吞吐）或 `mixed`（大块流并发叠加 1 条独立请求响应控制连接，`near` 自动预留 4 个活动 PCB 余量）；
- `direction`：`rx` 或 `tx`。

`build --profiles ...` 将构建清单绑定进内核 ELF；`run --variant label=kernel` 仅运行显式指定的内核（需要参照时传入 `--baseline` 或 `--linux-kernel`）。每次用例与重复均使用独立虚拟机启动和磁盘副本；只有接收端完整收齐全部字节并通过校验和才计为传输完成，混合用例还会额外校验控制连接在大流量期间是否真实持续推进。

---

## 四、统一比较与诊断纪律

1. **`smoke` 与正式 `run` 严格分离**：`smoke` 子命令仅验证功能通路正常；正式对比使用 `run` 至少完成 **3 次独立启动**，内核必须关闭 `COST_DIAGNOSTICS`，并与其他基准测试串行执行，避免宿主资源争抢。
2. **诊断独立采样**：`--observe` 使用单独的诊断内核采集完整内部计数快照，观测框架自身的额外开销通过配对的 ON/OFF 对照获得。
3. **指标边界不混加**：存储的页缓存接纳返回、`fsync`/`fdatasync` 落盘与卸载检查点属于三个不同完成边界；操作次数占比不等同于耗时占比，嵌套的中断、锁等待与睡眠计时不可直接相加。每文件完成时间分布用于刻画当前负载，不代表并发调度的饥饿上界。后续待优化项与路线见 [开发路线](../goals.md)。
