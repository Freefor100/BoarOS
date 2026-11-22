# procfs 控制文件（sysctl）与读写定位协议

在 Linux 体系中，`/proc`（进程文件系统，procfs）承载着两类行为截然不同的虚拟文件：

1. **状态快照类文件**（如 `/proc/meminfo`、`/proc/<pid>/stat`、`/proc/mounts`）：用于导出一段只读的格式化文本。内核在一次读取周期（read epoch）开始时生成完整快照并缓存于打开文件描述（Open File Description，OFD）上，供调用者分多次短读顺序消费；直到 `lseek(0)` 回到文件头，才重新采样。
2. **实时控制类文件**（主要是 `/proc/sys/...` 下的系统控制接口 `sysctl`，以及 `/proc/<pid>/comm`、`/proc/<pid>/coredump_filter` 等进程控制入口）：用于直接读取或修改内核运行参数。

如果把控制类文件硬塞进普通快照路径，短读、重复写、并发修改以及 `lseek` 行为都会产生严重偏差。本文基于 `fs/procfs.c`、`fs/files/io.c`、`fs/vfs.c` 与 `kernel/uts.c` 的实际代码，说明 BoarOS 为控制文件建立的独立读写、权限与定位协议。`procfs` 整体目录树结构见 [procfs 文件系统](../modules/procfs.md)。

---

## 一、控制节点分类与标记

在 `fs/procfs.c` 中，只有真正的控制文件会设置 `node->generated_control` 标志（分为读/写能力位与可选的前缀写上限 `control_write_prefix`），其余普通生成文件继续沿用 OFD 快照协议。当前纳入控制协议的节点分为四类：

| 类别 | 代表路径 | 解析/写入模式 |
| :--- | :--- | :--- |
| **单整数 `sysctl` 参数** | `/proc/sys/kernel/sched_rt_period_us`<br>`/proc/sys/kernel/sched_rt_runtime_us`<br>`/proc/sys/kernel/sched_rr_timeslice_ms`<br>`/proc/sys/kernel/unprivileged_bpf_disabled`<br>`/proc/sys/vm/nr_hugepages`、`/proc/sys/vm/drop_caches`、`/proc/sys/vm/vfs_cache_pressure`<br>`/proc/sys/fs/aio-nr`、`/proc/sys/fs/aio-max-nr` | 完整缓冲写 + `do_proc_dointvec` 风格整数解析；非零偏移读直接返回 EOF |
| **UTS 字符串 `sysctl`** | `/proc/sys/kernel/hostname`<br>`/proc/sys/kernel/domainname` | 完整缓冲写 + 按偏移续读/续写，每次实时读取当前 UTS 命名空间快照 |
| **有界前缀进程控制文件** | `/proc/<pid>/comm`、`/proc/<pid>/task/<tid>/comm`（15 字节上限，拒绝 `pwrite` 返回 `-ESPIPE`）<br>`/proc/<pid>/coredump_filter`（34 字节上限，支持 `pwrite`） | 按 `iovec` 分段仅导入有界前缀，不触碰被忽略尾部的用户页 |
| **诊断控制开关** | `/proc/boaros_cost_control`（仅开启 `COST_DIAGNOSTICS` 构建可见） | 写指令控制内置成本观测窗口启停 |

---

## 二、读取协议：拒绝陈旧快照与原子错误语义

`fs/files/io.c` 中的 `control_read()` 为控制文件实现了与普通快照完全分离的读取规则：

- **每次实时读取内核状态**：不在 OFD 上缓存历史文本，直接向 `proc_control()` 索取当前全局或命名空间状态。
- **单整数 `sysctl` 的零偏移规则**：对于所有单整数控制文件，只有在 `*offset == 0` 时才输出当前十进制数值与换行符；**只要文件偏移非零，立即返回 0（EOF）**。即使首次 `read` 只提供了 2 字节缓冲区导致截断，下一次 `read` 也不会续读剩余数字；调用者必须显式 `lseek(fd, 0, SEEK_SET)` 回到偏移 0 才能再次观察最新配置。
- **部分拷贝失败不推进偏移**：将格式化结果拷回用户缓冲区（`kernel_copy_to_user_iov()`）时，若任意字节触发用户态缺页错误，直接返回 `-EFAULT` 且**保持文件偏移不变**，不会像普通 `proc` 快照那样返回已拷贝的前缀字节数。

---

## 三、写入协议：全量导入与前缀有界两套路径

`fs/files/io.c` 中的 `control_write()` 根据 `node->control_write_prefix` 将写入分为两条精确语义路径：

### 3.1 `sysctl` 全量缓冲写入（`prefix == 0`）

Linux 的 `proc_sys_call_handler()` 在进入参数解析器之前，会先把用户提交的整个写入迭代器（包括后端不会消费的第二个 token、超出解析窗口的后缀，以及在非零偏移处最终会被忽略的输入）完整拷入内核缓冲区。

BoarOS 在文件层严格复现了这一行为：
1. 先执行控制写权限检查（详见第五节）；
2. 若写入长度非零，按总长度 `count` 分配内核缓冲并通过 `kernel_copy_from_user_iov()` 一次性收集全部 `iovec` 分段；
3. 若 `writev` 的第一段合法、第二段指向非法用户地址，拷贝阶段即返回 `-EFAULT`，**绝不会提前发布第一段中的数值或推进文件偏移**；
4. 完整收集成功后才调用后端 `proc_control()`，再按后端返回的实际消费长度与新位置更新 OFD 偏移。

### 3.2 单整数解析规则（对齐 `do_proc_dointvec`）

`fs/procfs.c` 对整数类 `sysctl` 写入采用与 Linux `do_proc_dointvec()` 一致的词法与长度规则：

- **非零偏移忽略**：若写入起始偏移 `*offset != 0`，数据已完成全量用户拷贝校验，直接视作成功消费全部字节并推进偏移，不修改参数。
- **解析窗口与进制**：即使输入超过一页，完整拷贝仍照常进行，但解析窗口截断为 **`PAGE_SIZE - 1`**；采用 base-0 数字规则（支持十进制、`0` 前缀八进制与 `0x`/`0X` 前缀十六进制），支持可选负号 `'-'`，**不接受正号 `'+'`**，单个数字词元最长不超过 20 个字符。
- **严格分隔符**：跳过前导空白后只解析一个整数；紧随数字后的首个字符若未到窗口末尾，只能是空格（`' '`）、制表符（`'\t'`）或换行符（`'\n'`），否则返回 `-EINVAL`。
- **已消费前缀计算**：数值交由对应子系统设置函数一次性完成合法性校验（例如调度器统一校验 `period`/`runtime` 配对约束，而非在 `procfs` 中硬编码特判）。提交成功后跳过紧随其后的空白，按 `consumed = size - (limit - position)` 计算返回值并推进偏移，不会把末尾未解析的第二个 token 误计为整体成功。

### 3.3 有界前缀写入（`prefix != 0`）

针对 `/proc/<pid>/comm`（上限 15 字节）和 `/proc/<pid>/coredump_filter`（上限 34 字节）两类传统进程控制文件，`control_write()` 逐段处理每个 `iovec`：每段最多只从用户态导入 `min(segment, prefix)` 字节，避免触发被忽略尾部的无效页错误，同时向上返回整段 `segment` 长度。其中 `comm` 未置位 `KERNEL_VFS_CONTROL_PREFIX_POSITIONED`，因此对它调用 `pwrite` 会直接返回 `-ESPIPE`。

---

## 四、UTS 字符串控制（`hostname` / `domainname`）

主机名（`/proc/sys/kernel/hostname`）与域名（`/proc/sys/kernel/domainname`）走同一套 `sysctl` 全量用户拷贝路径，但在偏移推进与字节保留上具有 Linux `_proc_do_string` 特有的精细语义（实现于 `fs/procfs.c` 与 `kernel/uts.c`）：

- **读路径支持偏移续读**：每次读取都对当前任务的 UTS 命名空间做短锁快照（`kernel_uts_snapshot()`），允许分多次短读沿偏移继续输出字段内容及末尾换行符；若偏移已超过当前字符串长度，则返回 0（EOF）。
- **写路径允许范围内续写**：扫描内核缓冲直至遇到 `NUL`（`'\0'`）、换行符（`'\n'`）或字段上限（`KERNEL_UTS_NAME_MAX`），在 UTS 命名空间短锁内（`kernel_uts_name_write()`）校验当前字符串长度：
  - 若 `offset <= strlen(field)`，从 `offset` 处覆写新内容并写入新的终止符 `'\0'`，然后推进文件偏移 `*offset += size`；
  - 若 `offset > strlen(field)`，忽略修改且**保持文件偏移不变**，但系统调用返回值仍然等于本次写入的完整输入长度 `size`。正因“返回值”与“新文件位置”可以解耦，`proc_control()` 通过独立参数分别返回 `count` 和更新后的 `*offset`，文件层绝不盲目按返回值累加偏移。
- **保留终止符之后的尾字节**：通过 `sethostname` / `setdomainname` 系统调用改名（`kernel_uts_name_set()`）会将字段剩余空间全部清零；而通过 `/proc/sys/kernel/{hostname,domainname}` 写入则遵循 Linux `_proc_do_string`，仅在写入末尾放置一个新的 `'\0'`，**保留该终止符之后原有的旧字节**——这些字节仍留在 `uname()` 返回的 390 字节定长结构中。

---

## 五、权限模型与 `lseek` 定位差异

### 5.1 有效 UID 判定与双阶段权限检查

对于 `hostname`、`domainname`、`nr_hugepages`、`aio-max-nr`、`aio-nr`、`vfs_cache_pressure` 与 `drop_caches` 等 `sysctl` 节点，`proc_permission()` 直接依据调用者当前的**有效用户标识（Effective UID，`euid`）**与控制表权限位（如 `0644`，`drop_caches` 为 `0200`）判断，**不使用普通 `inode` 的 `fsuid` 或自主访问控制绕过（DAC override）**：

- **打开阶段（`open`）**：权限不足返回 **`-EACCES`**。
- **每次读写阶段（`read` / `write`）**：`control_read()` 与 `control_write()` 在拷贝任何用户态数据之前，都会重新调用 `kernel_vfs_file_control_permission()` 复核当前凭据，权限不足时将 `-EACCES` 转换为 **`-EPERM`** 返回。这一设计保证了两点安全性质：
  1. 进程即便继承了父进程已打开的可写控制 FD，在降权后也无法修改内核参数；
  2. 传入非法用户缓冲区地址不会遮蔽前置的权限拒绝错误。
- 与之对应，直接修改主机名/域名的 `sethostname`/`setdomainname` 系统调用则按 `CAP_SYS_ADMIN` 能力位检查。

### 5.2 `lseek` 行为对照

`fs/files/io.c` 明确区分了普通生成文件（模拟 Linux `seq_file`）与控制文件（遵循 Linux `default_llseek`）的定位语义：

| `whence` 参数 | 普通 `proc` 快照文件（`seq_file`） | `proc` 控制文件（`default_llseek`） |
| :--- | :--- | :--- |
| `SEEK_SET` / `SEEK_CUR` | 按常规规则更新偏移；若跳至非零新偏移则重建快照 | 按常规规则更新偏移（不允许负目标偏移） |
| `SEEK_END` | 返回 **`-EINVAL`** | 视文件大小为 `0`，以 `0 + offset` 计算目标偏移（允许 `offset >= 0`） |
| `SEEK_DATA` / `SEEK_HOLE` | 非普通文件，返回 `-EINVAL` | 将 `i_size = 0` 视作文件尾空洞：`offset >= 0` 返回 **`-ENXIO`**；负偏移下 `SEEK_HOLE` 落回偏移 `0`，`SEEK_DATA` 仍因负目标返回 `-EINVAL` |

---

## 六、参考依据与验证

- **规范依据**：Linux 内核 `fs/proc/proc_sysctl.c`（`proc_sys_call_handler`）、`kernel/sysctl.c`（`do_proc_dointvec` / `_proc_do_string`）、`kernel/utsname_sysctl.c`（`proc_do_uts_string`）与 `fs/read_write.c`（`default_llseek`）。与 Linux 做双侧对照时，参考内核配置需开启 `CONFIG_SYSCTL` 与 `CONFIG_PROC_SYSCTL`。
- **同 ELF 差分测试**：`tests/diff-abi/rt_controls.c` 覆盖 32 位调度参数解析、非零偏移忽略、短读与 `lseek`、多段 `writev` 用户态 fault、降权写拒绝及只读挂载行为，并在 BoarOS 与参考 Linux 上执行同一份可执行文件进行逐项比对。
